#include "EncoderPipeline.h"
#include "media/TimedSource.h"
#include "model/Canvas.h"
#include "recording/EncoderRegistry.h"

#include <QDateTime>
#include <QEventLoop>
#include <QFileInfo>
#include <QImage>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#include <algorithm>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Tiny worker that blocks on ConnectNamedPipe so the GUI thread doesn't
// stall while waiting for ffmpeg to open the audio side of the pipe.
class PipeAcceptThread : public QThread {
    Q_OBJECT
public:
    explicit PipeAcceptThread(void* pipe, QObject* parent = nullptr)
        : QThread(parent), m_pipe(pipe) {}
signals:
    void connectedOk();
    void connectFailed();
protected:
    void run() override {
        BOOL ok = ConnectNamedPipe(static_cast<HANDLE>(m_pipe), nullptr);
        if (ok || GetLastError() == ERROR_PIPE_CONNECTED) emit connectedOk();
        else emit connectFailed();
    }
private:
    void* m_pipe;
};

// ---------------------------------------------------------------------------

EncoderPipeline::EncoderPipeline(QObject* parent) : QObject(parent) {
    // Locate ffmpeg.exe once at construction. MediaController disables the
    // record/stream buttons if this is empty.
    m_ffmpegPath = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
}

EncoderPipeline::~EncoderPipeline() {
    if (m_running) stop();
    cleanup();
}

namespace {
// Common input args: rawvideo from stdin + s16le PCM from named pipe.
// Always at canvas-native resolution (1920x1080); the scale filter in the
// output args resizes when OutputSettings differs.
QStringList buildInputArgs(const OutputSettings& s, const QString& audioPipeName) {
    const QString srcRes = QStringLiteral("%1x%2")
                               .arg(MalloyCanvas::Width).arg(MalloyCanvas::Height);
    return {
        QStringLiteral("-y"),
        QStringLiteral("-hide_banner"),
        QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-f"),       QStringLiteral("rawvideo"),
        QStringLiteral("-pix_fmt"), QStringLiteral("bgra"),
        QStringLiteral("-s"),       srcRes,
        QStringLiteral("-r"),       QString::number(s.fps),
        QStringLiteral("-i"),       QStringLiteral("pipe:0"),
        QStringLiteral("-f"),       QStringLiteral("s16le"),
        QStringLiteral("-ar"),      QStringLiteral("48000"),
        QStringLiteral("-ac"),      QStringLiteral("2"),
        QStringLiteral("-i"),       audioPipeName,
    };
}
} // namespace

QStringList EncoderPipeline::buildOutputArgs(const Target& target) const {
    const OutputSettings& s = target.output;
    QStringList args;

    // Scale filter when output differs from canvas native.
    if (s.width != MalloyCanvas::Width || s.height != MalloyCanvas::Height) {
        args << QStringLiteral("-vf")
             << QStringLiteral("scale=%1:%2").arg(s.width).arg(s.height);
    }

    // Defer to the per-encoder arg builder in EncoderRegistry. Hardware
    // encoders need different flags than libx264 — for example NVENC rejects
    // libx264 preset names like "veryfast"/"faster" with EINVAL (exit -22),
    // and uses `-preset p1..p7 -rc cbr -b:v <kbps>k` instead. The registry's
    // lambda emits the correct flags for whichever encoder is selected.
    const EncoderRegistry::Encoder* enc = EncoderRegistry::find(s.videoCodec);
    if (enc) {
        args << enc->buildArgs(s);
    } else {
        // Unknown codec id — fall back to libx264-style args. Should never
        // happen via the UI (the combo is populated from the registry), but
        // protects against corrupted QSettings or a project file from a
        // future build that listed an encoder this build doesn't know.
        args << QStringLiteral("-c:v")     << s.videoCodec
             << QStringLiteral("-preset")  << s.preset
             << QStringLiteral("-crf")     << QString::number(s.crf)
             << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
    }

    args << QStringLiteral("-c:a")     << s.audioCodec
         << QStringLiteral("-b:a")     << QStringLiteral("%1k").arg(s.audioBitratekbps)
         // NOTE: no `-shortest`. ffmpeg ends naturally when both stdin and
         // the audio pipe hit EOF during stop(). With `-shortest`, even a
         // brief startup stall on the audio pipe would terminate the output.
         << target.destination;

    return args;
}

bool EncoderPipeline::start(const Target& target,
                            TimedFrameSource* frames,
                            TimedPcmSource*   audio,
                            QString* error) {
    auto setErr = [&](const QString& m) { if (error) *error = m; };

    if (m_running) { setErr(QStringLiteral("Already running")); return false; }
    if (m_ffmpegPath.isEmpty()) { setErr(QStringLiteral("ffmpeg.exe not found in PATH")); return false; }
    if (!frames || !audio) { setErr(QStringLiteral("frame / audio source missing")); return false; }

    m_target    = target;
    m_frames    = frames;
    m_audio     = audio;
    m_pipeReady = false;

    // --- 1. Create the audio named pipe (server side, synchronous) ---
    const quint32 salt = QRandomGenerator::global()->generate();
    m_audioPipeName = QStringLiteral("\\\\.\\pipe\\malloy_audio_%1_%2")
                          .arg(static_cast<int>(GetCurrentProcessId()))
                          .arg(salt, 8, 16, QLatin1Char('0'));

    const std::wstring wname = m_audioPipeName.toStdWString();
    HANDLE pipe = CreateNamedPipeW(
        wname.c_str(),
        PIPE_ACCESS_OUTBOUND,                                // server writes
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,                                                   // max instances
        1024 * 1024,                                         // out buffer (1 MB)
        0,                                                   // in buffer
        0,                                                   // default timeout
        nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        setErr(QStringLiteral("CreateNamedPipe failed (err=%1)").arg(GetLastError()));
        return false;
    }
    m_audioPipe = pipe;

    // --- 2. Spawn ffmpeg with common input args + subclass-supplied output args ---
    QStringList args = buildInputArgs(m_target.output, m_audioPipeName);
    args << buildOutputArgs(m_target);

    m_ffmpeg = new QProcess(this);
    m_ffmpeg->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    connect(m_ffmpeg, &QProcess::errorOccurred, this, &EncoderPipeline::onFfmpegError);
    connect(m_ffmpeg, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus){ onFfmpegFinished(code); });

    m_ffmpeg->start(m_ffmpegPath, args);
    if (!m_ffmpeg->waitForStarted(3000)) {
        setErr(QStringLiteral("ffmpeg failed to start: %1").arg(m_ffmpeg->errorString()));
        cleanup();
        return false;
    }

    // --- 3. Wait for ffmpeg to attach to the audio pipe (off main thread) ---
    auto* accept = new PipeAcceptThread(m_audioPipe);
    m_pipeAcceptor = accept;
    connect(accept, &PipeAcceptThread::connectedOk, this, &EncoderPipeline::onPipeConnected);
    connect(accept, &PipeAcceptThread::connectFailed, this, &EncoderPipeline::onPipeConnectFailed);
    connect(accept, &QThread::finished, accept, &QObject::deleteLater);
    accept->start();

    // --- 4. Start pumping video immediately (audio waits for pipe connect) ---
    m_videoTimer = new QTimer(this);
    m_videoTimer->setTimerType(Qt::PreciseTimer);
    m_videoTimer->setInterval(1000 / std::max(1, m_target.output.fps));
    connect(m_videoTimer, &QTimer::timeout, this, &EncoderPipeline::onTickVideo);
    m_videoTimer->start();

    // --- 5. Watchdog: if ffmpeg never opens the audio pipe within 5 s,
    // it almost certainly crashed during arg parsing. Surface that as a
    // real error so the user isn't left staring at a frozen Record button.
    m_pipeWatchdog = new QTimer(this);
    m_pipeWatchdog->setSingleShot(true);
    m_pipeWatchdog->setInterval(5000);
    connect(m_pipeWatchdog, &QTimer::timeout, this, [this] {
        if (m_running && !m_pipeReady) {
            emit errorOccurred(QStringLiteral(
                "Audio pipe did not connect within 5 s — ffmpeg may have failed to start"));
            stop();
        }
    });
    m_pipeWatchdog->start();

    m_running = true;
    emit started();
    return true;
}

void EncoderPipeline::stop() {
    if (!m_running) return;
    m_running = false;

    if (m_videoTimer) {
        m_videoTimer->stop();
        m_videoTimer->deleteLater();
        m_videoTimer = nullptr;
    }

    if (m_pipeWatchdog) {
        m_pipeWatchdog->stop();
        m_pipeWatchdog->deleteLater();
        m_pipeWatchdog = nullptr;
    }

    if (m_audio) disconnect(m_audio, nullptr, this, nullptr);

    // Close stdin so ffmpeg knows the video stream is done.
    if (m_ffmpeg && m_ffmpeg->state() == QProcess::Running) {
        m_ffmpeg->closeWriteChannel();
    }

    // Disconnect the audio pipe so ffmpeg sees EOF on the audio side too.
    //
    // We deliberately DO NOT call FlushFileBuffers here. On a named pipe
    // FlushFileBuffers blocks until the *client* (ffmpeg) reads everything
    // the server has queued — but if ffmpeg has already started tearing down
    // its audio decoder during shutdown, it stops reading and the flush
    // never returns, hanging the GUI thread indefinitely.
    if (m_audioPipe) {
        DisconnectNamedPipe(static_cast<HANDLE>(m_audioPipe));
    }

    // Wait up to 5 s for ffmpeg to finalise the file, but keep the Qt event
    // loop spinning so the UI doesn't freeze ("Not Responding" badge). We
    // use a local QEventLoop that exits on QProcess::finished or on a 5 s
    // QTimer, whichever comes first.
    if (m_ffmpeg && m_ffmpeg->state() == QProcess::Running) {
        QEventLoop wait;
        QTimer killTimer;
        killTimer.setSingleShot(true);
        connect(m_ffmpeg, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                &wait, &QEventLoop::quit);
        connect(&killTimer, &QTimer::timeout, &wait, &QEventLoop::quit);
        killTimer.start(5000);
        wait.exec();
        if (m_ffmpeg->state() == QProcess::Running) {
            m_ffmpeg->kill();
            m_ffmpeg->waitForFinished(1000);
        }
    }

    const qint64 bytes = (m_target.kind == Target::Kind::File)
                              ? QFileInfo(m_target.destination).size()
                              : 0;
    const QString destination = m_target.destination;
    cleanup();
    emit finished(destination, bytes);
}

void EncoderPipeline::cleanup() {
    if (m_audioPipe) {
        CloseHandle(static_cast<HANDLE>(m_audioPipe));
        m_audioPipe = nullptr;
    }
    if (m_ffmpeg) {
        m_ffmpeg->deleteLater();
        m_ffmpeg = nullptr;
    }
    m_frames    = nullptr;
    m_audio     = nullptr;
    m_pipeReady = false;
    // m_pipeAcceptor self-deletes via finished signal
    m_pipeAcceptor = nullptr;
}

void EncoderPipeline::onTickVideo() {
    if (!m_running || !m_frames || !m_ffmpeg) return;
    if (m_ffmpeg->state() != QProcess::Running) return;

    const QImage frame = m_frames->currentFrame();
    if (frame.isNull()) return;

    // Convert to *straight-alpha* BGRA at canvas-native resolution. We declared
    // `-pix_fmt bgra` on stdin, which ffmpeg interprets as non-premultiplied.
    // PreviewWidget composes onto Format_ARGB32_Premultiplied for fast painting,
    // so we must force a conversion to Format_ARGB32 (Qt's un-premultiplies
    // correctly) — otherwise colors with alpha < 255 darken in the recording.
    QImage out = frame;
    if (out.format() != QImage::Format_ARGB32)
        out = out.convertToFormat(QImage::Format_ARGB32);
    if (out.width() != MalloyCanvas::Width || out.height() != MalloyCanvas::Height)
        out = out.scaled(MalloyCanvas::Width, MalloyCanvas::Height,
                         Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    // Single buffered write for the whole frame — Format_ARGB32 at 1920×1080
    // has no row padding (7680-byte rows on a 32-byte aligned buffer), so we
    // can ship the whole image in one QProcess::write() call. This avoids
    // partial-write error spam if ffmpeg dies mid-frame.
    const qint64 frameBytes = qint64(MalloyCanvas::Width) * MalloyCanvas::Height * 4;
    if (out.sizeInBytes() == frameBytes) {
        m_ffmpeg->write(reinterpret_cast<const char*>(out.constBits()), frameBytes);
    } else {
        // Defensive fall-back if Qt unexpectedly inserted scanline padding.
        const int rowBytes = MalloyCanvas::Width * 4;
        for (int y = 0; y < MalloyCanvas::Height; ++y) {
            m_ffmpeg->write(reinterpret_cast<const char*>(out.constScanLine(y)), rowBytes);
        }
    }
}

void EncoderPipeline::onMixedSamples(QByteArray pcm) {
    if (!m_running || !m_pipeReady) return;
    writeAudioBytes(pcm);
}

bool EncoderPipeline::writeAudioBytes(const QByteArray& pcm) {
    if (!m_audioPipe || pcm.isEmpty()) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(static_cast<HANDLE>(m_audioPipe),
                        pcm.constData(),
                        static_cast<DWORD>(pcm.size()),
                        &written, nullptr);
    if (!ok) {
        // Don't tear down here — ffmpeg might just be slow. Surface on next stop.
        return false;
    }
    return true;
}

void EncoderPipeline::onPipeConnected() {
    m_pipeReady = true;
    if (m_pipeWatchdog) m_pipeWatchdog->stop();   // cancel the 5 s timeout
    if (m_audio) {
        connect(m_audio, &TimedPcmSource::pcmReady,
                this,    &EncoderPipeline::onMixedSamples,
                Qt::QueuedConnection);
    }
}

void EncoderPipeline::onPipeConnectFailed() {
    if (!m_running) return;
    emit errorOccurred(QStringLiteral("Audio pipe did not connect to ffmpeg"));
    stop();
}

void EncoderPipeline::onFfmpegError() {
    if (!m_running) return;
    const QString msg = m_ffmpeg ? m_ffmpeg->errorString() : QStringLiteral("ffmpeg error");
    emit errorOccurred(QStringLiteral("ffmpeg: ") + msg);
    stop();
}

void EncoderPipeline::onFfmpegFinished(int exitCode) {
    if (!m_running) return; // expected — our stop() triggered it
    if (exitCode != 0) {
        emit errorOccurred(QStringLiteral("ffmpeg exited unexpectedly with code %1").arg(exitCode));
    }
    stop();
}

// PipeAcceptThread is a QObject defined in this .cpp file; AUTOMOC generates
// EncoderPipeline.moc to provide its meta-object code.
#include "EncoderPipeline.moc"
