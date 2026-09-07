#include "EncoderPipeline.h"
#include <QProcessEnvironment>
#include "media/TimedSource.h"
#include "model/Canvas.h"
#include "recording/EncoderRegistry.h"

#include <QDateTime>
#include <QEventLoop>
#include <QFileInfo>
#include <QImage>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringView>
#include <QThread>
#include <QTimer>

#include <algorithm>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <QMutex>
#include <QQueue>
#include <QWaitCondition>

#include <atomic>

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

// Writes PCM to the audio pipe on its own thread.
//
// The pipe is created PIPE_WAIT, so WriteFile blocks once its buffer is full
// and stays blocked until ffmpeg reads. Doing that on the GUI thread deadlocked
// the application outright: onTickVideo runs on the same thread, so a blocked
// audio write stopped video reaching ffmpeg's stdin, and ffmpeg then never got
// far enough to drain the audio pipe that the GUI thread was waiting on.
// Neither side could move, and both processes stayed alive looking healthy
// while the recording silently stopped growing.
//
// Blocking is fine here, on a thread whose only job is this write. When the
// encoder stalls for longer than the queue holds, audio is dropped rather than
// allowed to accumulate: a gap in the recording beats an unbounded buffer, and
// both beat a hung application.
class AudioPipeWriter : public QThread {
    Q_OBJECT
public:
    // ~30 s of 48 kHz stereo at the 20 ms chunk size the mixer emits, which is
    // under 6 MB.
    //
    // Deliberately generous. ffmpeg derives audio time from the byte count, so
    // a dropped chunk deletes time from the recording exactly as a dropped
    // video frame used to, and unlike video there is no timestamp trick that
    // recovers it. Holding the audio is cheap and keeps the timeline honest;
    // dropping is the last resort for a stall long enough that the recording
    // is already ruined.
    static constexpr int kMaxQueuedChunks = 1500;

    explicit AudioPipeWriter(void* pipe, QObject* parent = nullptr)
        : QThread(parent), m_pipe(pipe) {}

    void enqueue(const QByteArray& pcm) {
        QMutexLocker lock(&m_mutex);
        if (m_stopping) return;
        while (m_queue.size() >= kMaxQueuedChunks) {
            m_queue.dequeue();
            ++m_dropped;
        }
        m_queue.enqueue(pcm);
        m_wake.wakeOne();
    }

    // Asks the thread to finish. Does not block, and on its own is not enough:
    // the thread may be inside a WriteFile that returns only once the write
    // completes or is cancelled, so callers pair this with
    // unblockPendingWrite() before joining.
    void requestStop() {
        QMutexLocker lock(&m_mutex);
        m_stopping = true;
        m_wake.wakeAll();
    }

    // Cancels a WriteFile this thread is currently blocked in.
    //
    // Needed because the owner cannot break the pipe to free us:
    // DisconnectNamedPipe on a handle with a synchronous write pending from
    // another thread waits for that write, which is itself waiting for the
    // reader, so neither returns. CancelSynchronousIo targets the blocked
    // thread directly and is the supported way out.
    void unblockPendingWrite() {
        HANDLE self = static_cast<HANDLE>(m_selfHandle.load());
        if (self) CancelSynchronousIo(self);
    }

    ~AudioPipeWriter() override {
        HANDLE self = static_cast<HANDLE>(m_selfHandle.exchange(nullptr));
        if (self) CloseHandle(self);
    }

    int droppedChunks() const {
        QMutexLocker lock(&m_mutex);
        return m_dropped;
    }

protected:
    void run() override {
        // A real handle to this thread, so the owner can cancel a blocking
        // write from outside. GetCurrentThread() alone is a pseudo-handle and
        // means nothing to another thread.
        HANDLE self = nullptr;
        if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                            GetCurrentProcess(), &self, 0, FALSE,
                            DUPLICATE_SAME_ACCESS)) {
            m_selfHandle.store(self);
        }

        for (;;) {
            QByteArray chunk;
            {
                QMutexLocker lock(&m_mutex);
                while (m_queue.isEmpty() && !m_stopping)
                    m_wake.wait(&m_mutex);
                if (m_queue.isEmpty() && m_stopping) return;
                chunk = m_queue.dequeue();
            }

            // Partial writes are possible on a byte-mode pipe, so keep going
            // until the chunk is gone or the pipe fails.
            qint64 offset = 0;
            while (offset < chunk.size()) {
                DWORD written = 0;
                const BOOL ok = WriteFile(static_cast<HANDLE>(m_pipe),
                                          chunk.constData() + offset,
                                          static_cast<DWORD>(chunk.size() - offset),
                                          &written, nullptr);
                // A cancelled write reports ERROR_OPERATION_ABORTED and lands
                // here too, which is the intended exit during shutdown.
                if (!ok || written == 0) return;   // closed, broken or cancelled
                offset += written;
            }
        }
    }

private:
    void*               m_pipe;
    // Set once at thread start; read by the owning thread to cancel a write.
    std::atomic<void*>  m_selfHandle{nullptr};
    mutable QMutex      m_mutex;
    QWaitCondition      m_wake;
    QQueue<QByteArray>  m_queue;
    bool                m_stopping = false;
    int                 m_dropped = 0;
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
        // `-loglevel error` suppresses the periodic status line, which is the
        // only source of live bitrate, dropped frames and encode rate.
        // `-stats` asks for it anyway, without bringing back the rest of the
        // info-level noise; verified against ffmpeg 8.1.1, where `-loglevel
        // error` alone emits nothing at all.
        QStringLiteral("-stats"),
        QStringLiteral("-f"),       QStringLiteral("rawvideo"),
        QStringLiteral("-pix_fmt"), QStringLiteral("bgra"),
        QStringLiteral("-s"),       srcRes,
        // Stamp each frame with the time it arrived rather than with its index,
        // and deliberately do NOT declare -r on this input.
        //
        // rawvideo carries no timestamps of its own. Given -r, ffmpeg generates
        // them by frame index and assumes every frame sits exactly 1/fps after
        // the last, so a frame this application drops deletes time from the
        // recording instead of leaving a gap. The file then plays fast and ends
        // short: 144 seconds of wall clock produced a 109 second file, a
        // quarter missing, which is a broken recording even though the
        // container is valid.
        //
        // -r also wins over this option, which is why it is gone rather than
        // merely accompanied. Measured against ffmpeg 8.1.1 with a producer
        // that stalls for four seconds in the middle of an eight second run:
        // with -r the output was 3.81 s, without it exactly 8.00 s.
        //
        // The nominal rate the demuxer falls back to is cosmetic; the encoder
        // takes its rate control from the output arguments, and -g is computed
        // from OutputSettings rather than from this.
        QStringLiteral("-use_wallclock_as_timestamps"), QStringLiteral("1"),
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

    // Honour the timestamps the input carries instead of resampling to a
    // constant rate. Without this ffmpeg duplicates or discards frames to hit
    // -r exactly, which undoes the wall-clock stamping on the input side and
    // puts the shortened duration back. A dropped frame should leave a gap in
    // the timeline, matching what the dropped-frame counter reports.
    args << QStringLiteral("-fps_mode") << QStringLiteral("vfr");

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
    // A file follows the source; a stream holds its own cadence. See Cadence.
    m_cadence = (m_target.kind == Target::Kind::File) ? Cadence::FollowSource
                                                      : Cadence::ConstantRate;
    m_lastSentSequence = 0;
    m_backlogDrops = 0;
    m_idleTicks = 0;

    QStringList args = buildInputArgs(m_target.output, m_audioPipeName);
    args << buildOutputArgs(m_target);

    // Reset stderr-tail buffers each start (we reuse the same pipeline across
    // start/stop cycles via MediaController, but we want a clean diagnostic
    // window per session).
    m_stderrTail.clear();
    m_stderrPending.clear();

    m_ffmpeg = new QProcess(this);
    // FFREPORT makes ffmpeg write a log whose header is the entire command
    // line, destination URL included. Nothing here needs it.
    QProcessEnvironment childEnv = QProcessEnvironment::systemEnvironment();
    childEnv.remove(QStringLiteral("FFREPORT"));
    m_ffmpeg->setProcessEnvironment(childEnv);
    // SeparateChannels (not ForwardedErrorChannel) so we can read stderr in C++
    // and surface it in errorOccurred messages + parse ffmpeg's ~1 Hz progress
    // lines for the streaming status display. v6 forwarded stderr straight to
    // the parent console, which meant the user only ever saw the exit code.
    m_ffmpeg->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_ffmpeg, &QProcess::errorOccurred, this, &EncoderPipeline::onFfmpegError);
    connect(m_ffmpeg, &QProcess::readyReadStandardError,
            this,     &EncoderPipeline::onFfmpegStderrReady);
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
    // Order matters. The writer may be blocked inside WriteFile waiting for a
    // reader that is already shutting down, so ask it to stop, then break the
    // pipe to make that call return, and only then join. Joining first hangs
    // the GUI thread, which is the same failure this whole path exists to
    // avoid and is why FlushFileBuffers is not called here either.
    // Retire the writer completely before touching the pipe. Cancelling its
    // in-flight write is what makes this join bounded; disconnecting first
    // hangs, because the disconnect waits on the very write it would free.
    if (m_audioWriter) {
        m_audioWriter->requestStop();
        m_audioWriter->unblockPendingWrite();
        if (m_audioWriter->wait(3000)) delete m_audioWriter;
        m_audioWriter = nullptr;   // leaked rather than freed if it never exits
    }

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
    // Defensive: stop() normally does this, but cleanup() is also reachable
    // from failed starts where the writer may exist without a stop. Same
    // ordering rule as stop(): cancel the pending write, then join.
    if (m_audioWriter) {
        m_audioWriter->requestStop();
        m_audioWriter->unblockPendingWrite();
        if (m_audioWriter->wait(3000)) delete m_audioWriter;
        m_audioWriter = nullptr;
    }
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

bool EncoderPipeline::shouldWriteFrame(Cadence cadence, quint64 sourceSequence,
                                       quint64 lastSentSequence) {
    // A stream owes its ingest a frame every tick regardless of whether the
    // picture moved, so the latest one is repeated.
    if (cadence == Cadence::ConstantRate) return true;

    // A source that does not sequence its frames cannot say whether this one
    // is new, so the only safe reading is that it is.
    if (sourceSequence == TimedFrameSource::kUnsequenced) return true;

    // Otherwise the same sequence means the same picture, already recorded.
    return sourceSequence != lastSentSequence;
}

void EncoderPipeline::onTickVideo() {
    if (!m_running || !m_frames || !m_ffmpeg) return;
    if (m_ffmpeg->state() != QProcess::Running) return;

    // A null frame must still produce bytes. ffmpeg opens its inputs in order
    // and blocks in avformat_open_input until the first bytes arrive on each
    // one, and it prints nothing at all, on any channel, until every input is
    // open. Skipping a tick here therefore does not merely drop a frame: it can
    // stall the whole process before its transcode loop, silently, for the
    // length of the run. RingTimedFrameSource pre-fills a black frame for the
    // same reason.
    // Nothing new to record. The timer runs at the configured rate, but the
    // desktop produces frames at its own, and writing the same picture again
    // would be inventing media rather than capturing it. The wall-clock
    // timestamps on the input carry the gap, so the recording still lasts as
    // long as the session.
    //
    // A stream skips this test: its ingest expects frames at the negotiated
    // rate whether or not anything moved, so the latest picture is repeated.
    {
        const quint64 seq = m_frames->frameSequence();
        if (!shouldWriteFrame(m_cadence, seq, m_lastSentSequence)) {
            ++m_idleTicks;
            return;
        }
        m_lastSentSequence = seq;
    }

    QImage frame = m_frames->currentFrame();
    if (frame.isNull()) {
        if (m_blackFrame.isNull()) {
            m_blackFrame = QImage(m_frames->nativeWidth(), m_frames->nativeHeight(),
                                  QImage::Format_ARGB32);
            m_blackFrame.fill(Qt::black);
        }
        frame = m_blackFrame;
    }

    // Do not queue a frame ffmpeg has not asked for yet, and decide that before
    // paying for the conversion below.
    //
    // Each frame is about 8 MB uncompressed, so at 60 fps this pushes roughly
    // half a gigabyte a second into stdin, and at 120 a gigabyte. QProcess
    // buffers whatever the child has not read, in memory, without limit, so any
    // period where ffmpeg encodes slower than real time accumulates there for
    // the rest of the run. The recording keeps working while memory climbs and
    // nothing in the application can see it happening.
    //
    // Checking here rather than after the conversion matters as much as the
    // cap itself: convertToFormat and scaled each allocate a fresh full-size
    // image, so a dropped frame that was converted first still churns the
    // allocator at the full frame rate.
    //
    // Dropping beats blocking because this runs on the thread that also
    // composes and paints. A dropped frame costs one frame; blocking here
    // would freeze the preview and the interface.
    const qint64 canvasBytes = qint64(MalloyCanvas::Width) * MalloyCanvas::Height * 4;
    if (m_ffmpeg->bytesToWrite() > kMaxWriteBacklogFrames * canvasBytes) {
        ++m_backlogDrops;
        return;
    }

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

    const qint64 frameBytes = qint64(MalloyCanvas::Width) * MalloyCanvas::Height * 4;

    // Single buffered write for the whole frame — Format_ARGB32 at 1920×1080
    // has no row padding (7680-byte rows on a 32-byte aligned buffer), so we
    // can ship the whole image in one QProcess::write() call. This avoids
    // partial-write error spam if ffmpeg dies mid-frame.
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
    if (!m_audioWriter || pcm.isEmpty()) return false;
    // Hands off and returns immediately. The blocking WriteFile happens on the
    // writer's own thread; see AudioPipeWriter for why that separation is not
    // optional.
    m_audioWriter->enqueue(pcm);
    return true;
}

void EncoderPipeline::onPipeConnected() {
    m_pipeReady = true;
    if (m_pipeWatchdog) m_pipeWatchdog->stop();   // cancel the 5 s timeout

    // Only now is there a reader on the other end, so this is the earliest
    // point a write can succeed.
    m_audioWriter = new AudioPipeWriter(m_audioPipe);
    m_audioWriter->start();

    if (m_audio) {
        connect(m_audio, &TimedPcmSource::pcmReady,
                this,    &EncoderPipeline::onMixedSamples,
                Qt::QueuedConnection);
    }
}

void EncoderPipeline::onPipeConnectFailed() {
    if (!m_running) return;
    QString msg = QStringLiteral("Audio pipe did not connect to ffmpeg");
    if (!m_stderrTail.isEmpty())
        msg += QStringLiteral("\n\nLast stderr:\n") + m_stderrTail;
    emit errorOccurred(msg);
    stop();
}

void EncoderPipeline::onFfmpegError() {
    if (!m_running) return;
    const QString detail = m_ffmpeg ? m_ffmpeg->errorString() : QStringLiteral("ffmpeg error");
    QString msg = QStringLiteral("ffmpeg: ") + detail;
    if (!m_stderrTail.isEmpty())
        msg += QStringLiteral("\n\nLast stderr:\n") + m_stderrTail;
    emit errorOccurred(msg);
    stop();
}

void EncoderPipeline::onFfmpegFinished(int exitCode) {
    if (!m_running) return; // expected — our stop() triggered it
    if (exitCode != 0) {
        QString msg = QStringLiteral("ffmpeg exited unexpectedly with code %1").arg(exitCode);
        if (!m_stderrTail.isEmpty())
            msg += QStringLiteral("\n\nLast stderr:\n") + m_stderrTail;
        emit errorOccurred(msg);
    }
    stop();
}

// ---------------------------------------------------------------------------
// ffmpeg stderr handling
// ---------------------------------------------------------------------------

void EncoderPipeline::onFfmpegStderrReady() {
    if (!m_ffmpeg) return;
    const QByteArray raw = m_ffmpeg->readAllStandardError();
    if (raw.isEmpty()) return;

    const QString chunk = QString::fromLocal8Bit(raw);

    // Append to the bounded tail (used in errorOccurred() messages).
    appendStderrTail(chunk);

    // Split into completed lines for progress parsing. ffmpeg emits its progress
    // line with a trailing \r (in-place overwrite); separate ffmpeg messages
    // arrive with \n. We treat both as line terminators.
    m_stderrPending += chunk;
    int searchFrom = 0;
    for (int i = 0; i < m_stderrPending.size(); ++i) {
        const QChar ch = m_stderrPending.at(i);
        if (ch == QLatin1Char('\n') || ch == QLatin1Char('\r')) {
            if (i > searchFrom) {
                // By value, not a view into m_stderrPending. parseProgressLine
                // emits progress() synchronously, and any slot that spins the
                // event loop would re-enter this function and remove() from
                // under the view.
                parseProgressLine(m_stderrPending.mid(searchFrom, i - searchFrom));
            }
            searchFrom = i + 1;
        }
    }
    if (searchFrom > 0) m_stderrPending.remove(0, searchFrom);
    // Don't let pending grow unboundedly if ffmpeg ever stops emitting line
    // terminators (shouldn't happen, but be defensive).
    if (m_stderrPending.size() > 8192)
        m_stderrPending.remove(0, m_stderrPending.size() - 4096);
}

void EncoderPipeline::appendStderrTail(const QString& chunk) {
    // Redact before storing: this tail is attached to errorOccurred() and shown
    // to the user, so the key must not survive into it.
    // Only a stream URL carries a secret. A recording's destination is a file
    // path, and masking that would hide the very thing the error is about.
    const QString safe = redactDestination(
        chunk, m_target.kind == Target::Kind::Rtmp ? m_target.destination : QString());
    constexpr int kTailCap = 4096;
    m_stderrTail += safe;
    if (m_stderrTail.size() > kTailCap) {
        // Trim to the most recent kTailCap chars; align to a newline so the
        // surfaced tail starts on a clean line where possible.
        m_stderrTail.remove(0, m_stderrTail.size() - kTailCap);
        const int nl = m_stderrTail.indexOf(QLatin1Char('\n'));
        if (nl > 0 && nl < 256) m_stderrTail.remove(0, nl + 1);
    }
}

QString EncoderPipeline::redactDestination(QString text, const QString& destination) {
    if (destination.isEmpty() || text.isEmpty()) return text;

    // Only the last path segment of an RTMP URL is secret; keeping the server
    // visible is what makes an error message useful at all.
    const int slash = destination.lastIndexOf(QLatin1Char('/'));
    const QString secret = (slash >= 0 && slash + 1 < destination.size())
                               ? destination.mid(slash + 1)
                               : QString();

    if (!secret.isEmpty()) {
        QString masked = destination;
        masked.replace(slash + 1, secret.size(), QStringLiteral("***"));
        text.replace(destination, masked);
        // ffmpeg sometimes prints the stream name on its own. A very short
        // segment is not a key and replacing it blindly would mangle unrelated
        // words, so only redact one that is plausibly a credential.
        if (secret.size() >= 8) text.replace(secret, QStringLiteral("***"));
    } else {
        text.replace(destination, QStringLiteral("***"));
    }
    return text;
}

bool EncoderPipeline::tryParseProgressLine(QStringView line,
                                            int* bitrateKbps,
                                            int* droppedFrames,
                                            int* encodeFps) {
    // ffmpeg progress line format (one example):
    //   frame= 1234 fps= 60 q=23.0 size=  4096kB time=00:00:20.00 bitrate=1700.6kbits/s drop=0 speed=1.0x
    //
    // Match `bitrate=...kbits/s` and (optionally) `drop=...` independently —
    // some ffmpeg builds emit `dup=` ahead of `drop=`, others omit `drop=`
    // entirely during the first second. Bitrate is the must-have anchor.
    static const QRegularExpression kBitrateRe(
        QStringLiteral(R"(bitrate=\s*([\d.]+)kbits/s)"));
    static const QRegularExpression kDropRe(
        QStringLiteral(R"(drop=\s*(\d+))"));
    // Absent on the first line or two, while ffmpeg has no rate to report yet.
    static const QRegularExpression kFpsRe(
        QStringLiteral(R"(fps=\s*([\d.]+))"));

    const QString text = line.toString();
    const auto bitMatch = kBitrateRe.match(text);
    if (!bitMatch.hasMatch()) return false;

    if (bitrateKbps)
        *bitrateKbps = static_cast<int>(bitMatch.captured(1).toDouble() + 0.5);
    int drops = 0;
    const auto dropMatch = kDropRe.match(text);
    if (dropMatch.hasMatch()) drops = dropMatch.captured(1).toInt();
    if (droppedFrames) *droppedFrames = drops;

    int fps = 0;
    const auto fpsMatch = kFpsRe.match(text);
    if (fpsMatch.hasMatch())
        fps = static_cast<int>(fpsMatch.captured(1).toDouble() + 0.5);
    if (encodeFps) *encodeFps = fps;
    return true;
}

void EncoderPipeline::parseProgressLine(QStringView line) {
    int kbps = 0, drops = 0, fps = 0;
    if (tryParseProgressLine(line, &kbps, &drops, &fps))
        emit progress(kbps, drops, fps, m_backlogDrops);
}

// PipeAcceptThread is a QObject defined in this .cpp file; AUTOMOC generates
// EncoderPipeline.moc to provide its meta-object code.
#include "EncoderPipeline.moc"
