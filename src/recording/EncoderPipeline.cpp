#include "EncoderPipeline.h"
#include <QProcessEnvironment>
#include "media/TimedSource.h"
#include "model/Canvas.h"
#include "platform/FrameProfile.h"
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
#include <sddl.h>

#include <QMutex>
#include <QQueue>
#include <QWaitCondition>

#include <atomic>

// Who may open the pipes.
//
// A named pipe created with a null lpSecurityAttributes gets the default
// descriptor, and Microsoft documents that default as granting read access to
// the Everyone group and to the anonymous account. These two pipes carry the
// user's screen and their microphone, and they are created with
// PIPE_ACCESS_OUTBOUND, so read is exactly the access that matters: the
// default hands a live feed of both to any local process that opens the pipe
// before ffmpeg does.
//
// The name cannot be the defence. \\.\pipe\ is an enumerable directory, so a
// watcher sees the name the moment it exists, whatever it is called.
//
// So the descriptor is explicit and protected: full control to the system and
// to the user running this process, no entry for anyone else, and no inherited
// ACE able to widen it. Construction can fail, and when it does the caller
// must abandon the recording rather than fall back to the default, which is
// the thing being fixed.
class PipeSecurity {
public:
    PipeSecurity() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return;

        DWORD needed = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
        LPWSTR sidText = nullptr;
        if (needed > 0) {
            QByteArray buffer(int(needed), Qt::Uninitialized);
            if (GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed)) {
                const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.constData());
                ConvertSidToStringSidW(user->User.Sid, &sidText);
            }
        }
        CloseHandle(token);
        if (!sidText) return;

        const std::wstring sddl =
            L"D:P(A;;GA;;;SY)(A;;GA;;;" + std::wstring(sidText) + L")";
        LocalFree(sidText);

        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(), SDDL_REVISION_1, &m_descriptor, nullptr)) {
            m_descriptor = nullptr;
            return;
        }
        m_attributes.nLength              = sizeof(m_attributes);
        m_attributes.lpSecurityDescriptor = m_descriptor;
        m_attributes.bInheritHandle       = FALSE;
    }

    ~PipeSecurity() {
        if (m_descriptor) LocalFree(m_descriptor);
    }

    PipeSecurity(const PipeSecurity&)            = delete;
    PipeSecurity& operator=(const PipeSecurity&) = delete;

    bool valid() const { return m_descriptor != nullptr; }

    // Never null when valid(), and never used when not: a null here would mean
    // the permissive default.
    SECURITY_ATTRIBUTES* attributes() { return m_descriptor ? &m_attributes : nullptr; }

private:
    PSECURITY_DESCRIPTOR m_descriptor = nullptr;
    SECURITY_ATTRIBUTES  m_attributes{};
};

// ---------------------------------------------------------------------------
// One video frame on the wire.
//
// Two threads write frames: the transport, for every ordinary frame, and the
// accept thread, for the frame that primes the pipe. They have to serialise
// identically rather than merely similarly, because rawvideo carries no
// framing whatsoever. The far end counts bytes and nothing else, so a frame
// written by different rules than the rest does not corrupt only itself: it
// shifts every frame after it for the length of the recording, and the result
// looks like a decoder bug rather than a writer bug.
// ---------------------------------------------------------------------------
namespace {

// Every byte, or a failure. A pipe may take a write in pieces, so a short
// write is ordinary and only a refusal is fatal.
bool writeAllToPipe(void* pipe, const char* data, qint64 bytes) {
    qint64 offset = 0;
    while (offset < bytes) {
        DWORD written = 0;
        const BOOL ok = WriteFile(static_cast<HANDLE>(pipe), data + offset,
                                  static_cast<DWORD>(bytes - offset), &written, nullptr);
        if (!ok || written == 0) return false;
        offset += written;
    }
    return true;
}

// Serialises one frame into the pipe.
//
// Checked, never corrected. Converting here would hide a caller that had
// stopped honouring the contract, and would spend a full frame of work on
// whichever thread happened to be writing.
bool writeFrameToPipe(void* pipe, const QImage& frame) {
    if (!EncoderPipeline::conformsToPipeDeclaration(frame)) return false;

    // Rows go one at a time only when Qt has padded them; a tightly packed
    // image is a single call. Asked of the image rather than inferred from its
    // total size, because the property that matters is the stride, and the
    // stride is precisely what rawvideo has no way to express.
    //
    // constBits and constScanLine, never bits or scanLine: the non-const
    // accessors detach, which would copy eight megabytes on the writing thread
    // and defeat the point of handing the frame over instead of copying it.
    const qint64 rowBytes = qint64(frame.width()) * (frame.depth() / 8);
    if (EncoderPipeline::isTightlyPacked(frame))
        return writeAllToPipe(pipe, reinterpret_cast<const char*>(frame.constBits()),
                              rowBytes * frame.height());
    for (int y = 0; y < frame.height(); ++y)
        if (!writeAllToPipe(pipe, reinterpret_cast<const char*>(frame.constScanLine(y)),
                            rowBytes))
            return false;
    return true;
}

}  // namespace

// Tiny worker that blocks on ConnectNamedPipe so the GUI thread doesn't
// stall while waiting for ffmpeg to open the audio side of the pipe.
class PipeAcceptThread : public QThread {
    Q_OBJECT
public:
    // Optionally writes a first payload as soon as the pipe is connected.
    //
    // ffmpeg opens its inputs in order and each open blocks until bytes
    // arrive, so nothing reaches the second input until the first has
    // produced some. Video is the first input, and its writer is not created
    // until the connection is signalled across to the thread that owns the
    // pipeline; if that thread is busy, the first video byte is late, ffmpeg
    // never gets as far as opening the audio pipe, and the audio watchdog
    // fails a recording that was about to work.
    //
    // Priming here removes the dependency: the bytes go out on this thread,
    // the moment there is a reader, whatever else is happening.
    explicit PipeAcceptThread(void* pipe, QImage priming = {}, QObject* parent = nullptr)
        : QThread(parent), m_pipe(pipe), m_priming(std::move(priming)) {}

    void requestStop() { m_stopping.store(true, std::memory_order_relaxed); }

    // A synchronous ConnectNamedPipe or WriteFile ends only when the thread
    // that issued it cancels it. Both writers already own this; without it
    // here, an ffmpeg that dies during argument parsing leaves this thread
    // blocked forever on a pipe the pipeline is about to close underneath it.
    void unblockPendingIo() {
        HANDLE self = static_cast<HANDLE>(m_selfHandle.load(std::memory_order_relaxed));
        if (self) CancelSynchronousIo(self);
    }

    ~PipeAcceptThread() override {
        HANDLE self = static_cast<HANDLE>(m_selfHandle.exchange(nullptr));
        if (self) CloseHandle(self);
    }
signals:
    // Carries how many video frames this thread put on the wire before any
    // transport existed: one for a primed pipe, zero otherwise. It travels with
    // the signal rather than being read back off this object afterwards, so the
    // transport can be opened with a truthful count without anyone having to
    // reason about whether this thread is still alive.
    void connectedOk(int primedFrames);
    void connectFailed();
protected:
    void run() override {
        HANDLE self = nullptr;
        if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                            GetCurrentProcess(), &self, 0, FALSE,
                            DUPLICATE_SAME_ACCESS)) {
            m_selfHandle.store(self, std::memory_order_relaxed);
        }
        // A stop can be requested before this thread reaches the connect, in
        // which case there is no I/O to cancel and only the flag catches it.
        if (stopping()) return;

        BOOL ok = ConnectNamedPipe(static_cast<HANDLE>(m_pipe), nullptr);
        if (!ok && GetLastError() != ERROR_PIPE_CONNECTED) {
            // A cancelled connect is a teardown, not a failure to report.
            if (!stopping()) emit connectFailed();
            return;
        }
        if (stopping()) return;

        // No check on which process connected, deliberately.
        //
        // Identifying the client by process id does not survive contact with
        // how ffmpeg is actually installed. Chocolatey, scoop and winget all
        // publish a shim that launches the real binary as a child, so the id
        // this process launched is the shim's and the id that opens the pipe is
        // its child's, and an exact match refuses a perfectly good encoder.
        // Walking the parent chain instead is no better, because a launcher
        // that exits once it has spawned leaves no chain to walk and the same
        // refusal follows, this time only on some machines.
        //
        // Refusing to record is a worse outcome than the race it would close,
        // and the race it would close is a process already running as this user.
        // What keeps other users, the anonymous account and remote clients out
        // is the descriptor on the pipe, which does not depend on guessing who
        // the client is. Removing the race entirely means not publishing a name
        // at all: see the inherited-handle issue.
        int primed = 0;
        if (!m_priming.isNull()) {
            // A failure here means the reader went away between connecting and
            // reading, which the pipeline discovers on its own first write.
            // Reporting nothing written keeps the frame count honest either
            // way, and the warning says which of the two happened.
            if (writeFrameToPipe(m_pipe, m_priming)) primed = 1;
            else if (!stopping()) qWarning("video pipe priming frame was not written");
        }
        if (stopping()) return;
        emit connectedOk(primed);
    }
private:
    bool stopping() const { return m_stopping.load(std::memory_order_relaxed); }

    void*              m_pipe;
    QImage             m_priming;
    std::atomic<bool>  m_stopping{false};
    std::atomic<void*> m_selfHandle{nullptr};
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


// Writes composed frames to ffmpeg's video pipe on its own thread.
//
// Video used to go to ffmpeg's stdin through QProcess. QProcess accepts a
// write into a buffer of its own and moves it into the pipe from the Qt event
// loop, and the thread running that loop is the one that composes frames,
// paints the preview and receives every captured frame. When it got busy it
// stopped draining its own buffer, the backlog pinned at the cap, and the
// pipeline refused nearly everything while ffmpeg sat idle waiting for bytes
// nobody was sending it. Measured against a consumer that could take 945 MB/s:
// removing event loop service turned that into 2 MB/s, pinned the queue for
// 14.9 of 15 seconds and refused 896 of 900 frames.
//
// So the transport gets a thread of its own, exactly as the audio path already
// does, and for the same reason: the write may block, and the thread that
// produces the media must never be the thread that waits.
//
// The bound stays. This is not an unlimited escape hatch: a full queue refuses
// the frame immediately and the producer counts it, which is the behaviour the
// backpressure policy has always had.
class VideoPipeWriter : public QThread {
    Q_OBJECT
public:
    // The same three frames the QProcess backlog cap allowed, about 25 MB at
    // 1080p. Enough to ride out scheduling jitter, small enough that a stalled
    // encoder costs frames rather than memory.
    static constexpr int kMaxQueuedFrames = 3;

    // alreadyWritten is what reached the pipe before this thread existed: the
    // priming frame, where there was one. It is counted here rather than
    // ignored because it is a real completed write of real media, and a
    // transport that reported everything except the first frame would make
    // PIPE WRITE quietly false in every recording.
    explicit VideoPipeWriter(void* pipe, int alreadyWritten = 0, QObject* parent = nullptr)
        : QThread(parent), m_pipe(pipe), m_written(alreadyWritten) {}

    // Hands a frame to the transport, or refuses it. Never blocks: the caller
    // is the thread that composes, and it must not wait on a pipe.
    //
    // The image is passed by value and stored as one. QImage is implicitly
    // shared, so this transfers ownership without copying eight megabytes,
    // which is one full frame copy less than the old path paid on the GUI
    // thread.
    //
    // The invariant that makes that true: once a frame is accepted here, its
    // pixel storage is not written to by anyone until the writer has finished
    // with it. Sharing is not ownership, and if the producer kept a reference
    // and modified it, Qt would detach and pay for the copy after all, on
    // whichever thread touched it. Today the producer hands over the result of
    // convertToFormat, which is a fresh buffer nothing else holds, and moves
    // it; and this thread only ever reads through const accessors. Both halves
    // are required.
    bool trySubmit(QImage frame) {
        // The shape is a contract with ffmpeg, agreed when the arguments were
        // built, and a mismatch here would be encoded as though it matched.
        // Refused rather than corrected: correcting would put a full frame of
        // work back on the thread this class exists to keep free, and would
        // hide a caller that had stopped honouring the contract.
        //
        // Refusing at admission rather than at the write is what lets the
        // caller count the frame as dropped and lets the writer treat any later
        // failure as the pipe rather than the picture.
        if (!EncoderPipeline::conformsToPipeDeclaration(frame)) {
            if (!m_warnedFormat.exchange(true)) {
                qWarning("video transport refused a %dx%d frame in format %d; the pipe "
                         "was declared as %dx%d in format %d",
                         frame.width(), frame.height(), int(frame.format()),
                         int(MalloyCanvas::Width), int(MalloyCanvas::Height),
                         int(EncoderPipeline::rawVideoFormat()));
            }
            return false;
        }

        QMutexLocker lock(&m_mutex);
        if (m_stopping) return false;
        if (m_queue.size() >= kMaxQueuedFrames) return false;
        m_queue.enqueue(std::move(frame));
        m_wake.wakeOne();
        return true;
    }

    // Frames waiting to be written, and the bytes they represent. The queue
    // depth is what the backlog figure now reports.
    int queuedFrames() const {
        QMutexLocker lock(&m_mutex);
        return m_queue.size();
    }
    qint64 queuedBytes() const {
        QMutexLocker lock(&m_mutex);
        qint64 total = 0;
        for (const QImage& f : m_queue) total += f.sizeInBytes();
        return total;
    }

    // Video frames that have reached the pipe: every one this thread finished
    // writing, plus the priming frame the accept thread wrote before this
    // thread existed. Distinct from what the pipeline accepted, because
    // acceptance is a promise to carry a frame and this is the carrying, and
    // the gap between them is the transport's own backlog.
    //
    // The prime is counted here and deliberately not in ENC ACCEPT. It is a
    // completed write of real media, so PIPE WRITE owes it; it never passed
    // through the bounded admission this class offers, so ENC ACCEPT does not.
    int framesWritten() const { return m_written.load(std::memory_order_relaxed); }

    void requestStop() {
        QMutexLocker lock(&m_mutex);
        m_stopping = true;
        m_wake.wakeAll();
    }

    // Same reasoning as the audio writer: a synchronous WriteFile can only be
    // broken by cancelling it on the thread that issued it.
    void unblockPendingWrite() {
        HANDLE self = static_cast<HANDLE>(m_selfHandle.load());
        if (self) CancelSynchronousIo(self);
    }

    ~VideoPipeWriter() override {
        HANDLE self = static_cast<HANDLE>(m_selfHandle.exchange(nullptr));
        if (self) CloseHandle(self);
    }

protected:
    void run() override {
        HANDLE self = nullptr;
        if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                            GetCurrentProcess(), &self, 0, FALSE,
                            DUPLICATE_SAME_ACCESS)) {
            m_selfHandle.store(self);
        }

        for (;;) {
            QImage frame;
            {
                QMutexLocker lock(&m_mutex);
                while (m_queue.isEmpty() && !m_stopping)
                    m_wake.wait(&m_mutex);
                if (m_queue.isEmpty() && m_stopping) return;
                frame = m_queue.dequeue();
            }
            if (frame.isNull()) continue;

            FrameProfile::Scoped timing(FrameProfile::Stage::PipeWrite);

            // Everything in the queue conformed at admission, so a refusal here
            // is the pipe: closed, broken, or the write was cancelled.
            if (!writeFrameToPipe(m_pipe, frame)) return;
            m_written.fetch_add(1, std::memory_order_relaxed);
        }
    }

private:
    void*              m_pipe;
    std::atomic<bool>  m_warnedFormat{false};
    std::atomic<void*> m_selfHandle{nullptr};
    mutable QMutex     m_mutex;
    QWaitCondition     m_wake;
    QQueue<QImage>     m_queue;
    bool               m_stopping = false;
    std::atomic<int>   m_written{0};
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
QStringList buildInputArgs(const OutputSettings& s, const QString& videoPipeName,
                           const QString& audioPipeName) {
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
        // Declare the rate, and let the wall clock decide the timestamps.
        //
        // These are two different things and only the second was ever right
        // here. -framerate tells the demuxer what rate this stream nominally
        // runs at, which is what sets the time base; -r generates timestamps
        // from the frame index, which is what destroys a recording. Given -r,
        // ffmpeg assumes every frame sits exactly 1/fps after the last, so a
        // frame this application drops deletes time from the file instead of
        // leaving a gap: 144 seconds of wall clock produced a 109 second file,
        // and a producer stalled for four seconds inside an eight second run
        // produced 3.81 s. So -r stays gone.
        //
        // -framerate does not do that, because -use_wallclock_as_timestamps
        // still decides where each frame sits. Measured against ffmpeg 8.1.1
        // with the same stalling producer: 7.93 s of an eight second run, the
        // gap intact, against 3.81 s for -r.
        //
        // Leaving the rate undeclared is not neutral either. ffmpeg falls back
        // to 25, and that fallback was the output frame rate of every
        // recording this application has ever made: arrivals closer together
        // than 40 ms landed on one timestamp and -fps_mode vfr kept one of
        // them. See declaredInputFrameRate.
        QStringLiteral("-framerate"),
        QString::number(EncoderPipeline::declaredInputFrameRate(s)),
        QStringLiteral("-use_wallclock_as_timestamps"), QStringLiteral("1"),
        // A named pipe rather than stdin, so this side of it can be owned by a
        // thread of our own. See VideoPipeWriter for what stdin cost.
        QStringLiteral("-i"),       videoPipeName,
        QStringLiteral("-f"),       QStringLiteral("s16le"),
        QStringLiteral("-ar"),      QStringLiteral("48000"),
        QStringLiteral("-ac"),      QStringLiteral("2"),
        QStringLiteral("-i"),       audioPipeName,
    };
}
} // namespace

bool EncoderPipeline::conformsToPipeDeclaration(const QImage& frame) {
    return frame.format() == rawVideoFormat()
           && frame.width() == int(MalloyCanvas::Width)
           && frame.height() == int(MalloyCanvas::Height);
}

bool EncoderPipeline::isTightlyPacked(const QImage& frame) {
    if (frame.isNull()) return false;
    // depth() is in bits per pixel, and rawvideo wants exactly that many bytes
    // per pixel per row with no padding between rows.
    const qint64 rowBytes = qint64(frame.width()) * (frame.depth() / 8);
    return frame.bytesPerLine() == rowBytes;
}

void EncoderPipeline::noteFrameRejected() {
    // Only reached when a composition advanced and was then refused, since an
    // unchanged picture returns as an idle tick before this point.
    if (!m_inDropBurst) {
        m_inDropBurst = true;
        m_dropBurstClock.start();
    }
}

void EncoderPipeline::noteFrameAccepted() {
    closeDropBurst();
}

void EncoderPipeline::closeDropBurst() {
    if (!m_inDropBurst) return;
    m_longestDropBurstMs = std::max(m_longestDropBurstMs, m_dropBurstClock.elapsed());
    m_inDropBurst = false;
}

int EncoderPipeline::declaredInputFrameRate(const OutputSettings& settings) {
    // 1000 is an upper bound rather than a target: past it the time base gets
    // fine enough that the rate control on the stream path has nothing left to
    // spend per frame, and no sink here is clocked anywhere near it.
    return std::clamp(settings.fps, 1, 1000);
}

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
        // A file holds quality and lets its size follow the content. It is
        // also the only rate control that does not lose bitrate to the rate
        // declared on the input, which a file has no reason to pay.
        args << enc->buildArgs(s, EncoderRegistry::Destination::File);
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

    // --- 1. Create the two named pipes (server side, synchronous) ---
    //
    // Video has one now as well as audio. It used to go to ffmpeg's stdin
    // through QProcess, whose buffer only drains when the Qt event loop runs,
    // and that loop belongs to the thread composing the frames. See
    // VideoPipeWriter.
    // The descriptor is built once and applied to both pipes. If it cannot be
    // built the recording does not start: continuing would mean creating the
    // pipes with the permissive default, which is the vulnerability.
    PipeSecurity pipeSecurity;
    if (!pipeSecurity.valid()) {
        setErr(QStringLiteral("Could not secure the encoder pipes (err=%1)")
                   .arg(GetLastError()));
        cleanup();
        return false;
    }

    // system(), not global(): global() is the seeded non-cryptographic
    // generator. The name is defence in depth rather than the control, but a
    // predictable one would let a watcher wait on the name instead of polling.
    const quint32 salt = QRandomGenerator::system()->generate();
    auto makePipe = [&](const QString& kind, int outBufferBytes,
                        QString* nameOut, void** handleOut) -> bool {
        const QString name = QStringLiteral("\\\\.\\pipe\\malloy_%1_%2_%3")
                                 .arg(kind)
                                 .arg(static_cast<int>(GetCurrentProcessId()))
                                 .arg(salt, 8, 16, QLatin1Char('0'));
        const std::wstring wname = name.toStdWString();
        HANDLE pipe = CreateNamedPipeW(
            wname.c_str(),
            PIPE_ACCESS_OUTBOUND,                                // server writes
            // Remote clients rejected: without this the same pipe is reachable
            // over SMB as \\host\pipe\malloy_...
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1,                                                   // max instances
            static_cast<DWORD>(outBufferBytes),
            0,                                                   // in buffer
            0,                                                   // default timeout
            pipeSecurity.attributes());
        if (pipe == INVALID_HANDLE_VALUE) return false;
        *nameOut = name;
        *handleOut = pipe;
        return true;
    };

    // One frame of outbound buffer for video, so the operating system can hold
    // a whole picture while ffmpeg gets to it. Anything past that waits in the
    // writer's own bounded queue, where it can be counted.
    const int videoBuffer = static_cast<int>(qint64(MalloyCanvas::Width) * MalloyCanvas::Height * 4);
    if (!makePipe(QStringLiteral("video"), videoBuffer, &m_videoPipeName, &m_videoPipe) ||
        !makePipe(QStringLiteral("audio"), 1024 * 1024, &m_audioPipeName, &m_audioPipe)) {
        setErr(QStringLiteral("CreateNamedPipe failed (err=%1)").arg(GetLastError()));
        cleanup();
        return false;
    }

    // --- 2. Spawn ffmpeg with common input args + subclass-supplied output args ---
    // A file follows the source; a stream holds its own cadence. See Cadence.
    m_cadence = (m_target.kind == Target::Kind::File) ? Cadence::FollowSource
                                                      : Cadence::ConstantRate;
    m_lastSentSequence = 0;
    m_composedFramesRejected = 0;
    m_composedFramesAccepted = 0;
    m_cfrDuplicates = 0;
    m_idleTicks = 0;
    m_longestDropBurstMs = 0;
    m_inDropBurst = false;
    // The capture side's counters run for as long as the application has been
    // capturing, which is not the same span as this recording. Taking the
    // reading here is what makes the summary's source figures belong to this
    // run.
    m_sourceStatsAtStart = m_sourceStats ? m_sourceStats() : CaptureStats{};

    // Stage timings belong to the run being measured, the same way the
    // source counters do.
    if (FrameProfile::enabled()) {
        FrameProfile::reset();
        m_tickClock.invalidate();
    }

    QStringList args = buildInputArgs(m_target.output, m_videoPipeName, m_audioPipeName);
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

    // --- 3. Wait for ffmpeg to attach to both pipes (off main thread) ---
    //
    // ffmpeg opens its inputs in order and blocks on each until the server
    // side accepts, so both acceptors have to be waiting at once.
    // One frame, ready before ffmpeg is asked for anything, so its first input
    // can open without waiting on this thread.
    //
    // This frame is media, not a transport trick. rawvideo has no bytes that
    // are not picture, so anything written here becomes the recording's first
    // frame, and ffmpeg stamps it from the wall clock when it reads it, the
    // same as every other frame. It is therefore the current composition where
    // there is one, and black only when nothing has been composed yet, which
    // is what the tick below would have sent in the same situation.
    //
    // Its sequence is taken before the picture rather than after. If a new
    // composition lands between the two reads, the sequence recorded is the
    // older one and the next tick simply sends the newer picture; taking them
    // the other way round could record a sequence newer than the frame
    // actually sent and silently skip a composition.
    //
    // Counted as a write and not as an acceptance. It is a completed write of
    // real media, so PIPE WRITE includes it once the transport opens; it never
    // passed through the bounded admission that ENC ACCEPT describes, so that
    // counter is left alone. The relationship is then stateable rather than
    // approximate: PIPE WRITE is the ordinary completed writes plus a
    // successful prime, and no recording carries a silent extra frame.
    const quint64 primedSequence = m_frames ? m_frames->compositionSequence() : 0;
    QImage priming = m_frames ? m_frames->currentFrame() : QImage();
    if (priming.isNull()) {
        priming = QImage(int(MalloyCanvas::Width), int(MalloyCanvas::Height),
                         rawVideoFormat());
        priming.fill(Qt::black);
    } else {
        // The same preparation an ordinary frame gets in onTickVideo, for the
        // same reasons: the declared pixel format, and the declared resolution.
        // Format alone is not enough, because a first frame of the right format
        // and the wrong size is written without complaint and displaces every
        // frame after it.
        if (priming.format() != rawVideoFormat())
            priming = priming.convertToFormat(rawVideoFormat());
        if (priming.width() != int(MalloyCanvas::Width)
            || priming.height() != int(MalloyCanvas::Height))
            priming = priming.scaled(int(MalloyCanvas::Width), int(MalloyCanvas::Height),
                                     Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

        // A private buffer, unconditionally. The queue can rely on its frames
        // being fresh conversions that nothing else holds, but this one may be
        // the compositor's own image where no conversion was needed, and it is
        // about to be read on another thread while composition carries on.
        // Detaching costs one frame copy, once, before the recording starts.
        priming = priming.copy();
    }

    // The primed picture has been sent, so the tick that follows must not send
    // it again. Without this the recording opens with the same image twice, a
    // few milliseconds apart, because the sink still believed it had sent
    // nothing.
    m_lastSentSequence = primedSequence;

    // No deleteLater on these: they own a blocking wait on a pipe handle this
    // object closes, so stop() joins them explicitly instead.
    auto* videoAccept = new PipeAcceptThread(m_videoPipe, std::move(priming));
    m_videoAcceptor = videoAccept;
    connect(videoAccept, &PipeAcceptThread::connectedOk,
            this, &EncoderPipeline::onVideoPipeConnected);
    connect(videoAccept, &PipeAcceptThread::connectFailed,
            this, &EncoderPipeline::onPipeConnectFailed);
    videoAccept->start();

    auto* accept = new PipeAcceptThread(m_audioPipe);
    m_pipeAcceptor = accept;
    connect(accept, &PipeAcceptThread::connectedOk, this, &EncoderPipeline::onPipeConnected);
    connect(accept, &PipeAcceptThread::connectFailed, this, &EncoderPipeline::onPipeConnectFailed);
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

void EncoderPipeline::setSourceStatsProvider(std::function<CaptureStats()> provider) {
    m_sourceStats = std::move(provider);
}

void EncoderPipeline::stop() {
    if (!m_running || m_stopping) return;
    // isRunning() reports true for as long as this flag is set. Finalising
    // spins a nested event loop below, so the interface stays live while this
    // frame is on the stack, and a second Stop or a fresh Start arriving in
    // that window would otherwise find isRunning() already false and delete
    // this object from under the loop.
    m_stopping = true;
    m_running  = false;

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

    // The acceptors retire first, and for a sharper reason than tidiness. One
    // may be blocked in ConnectNamedPipe waiting for an ffmpeg that died during
    // argument parsing, or part way through the priming write. DisconnectNamedPipe
    // below would then wait on that write while the write waits for a reader:
    // the deadlock AudioPipeWriter::unblockPendingWrite already documents.
    // Cancelling their I/O is what makes these joins bounded, and joining them
    // is what stops cleanup() closing a handle another thread is blocked on.
    const auto retireAcceptor = [](PipeAcceptThread*& thread) {
        if (!thread) return;
        thread->requestStop();
        thread->unblockPendingIo();
        if (thread->wait(3000)) delete thread;
        thread = nullptr;   // leaked rather than freed if it never exits
    };
    retireAcceptor(m_videoAcceptor);
    retireAcceptor(m_pipeAcceptor);

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

    // The video transport retires the same way and for the same reason: ask it
    // to finish, cancel the write it may be blocked inside, then join with a
    // bound. Frames still queued are abandoned deliberately rather than
    // drained, because stopping should not wait on an encoder that has already
    // stopped reading.
    if (m_videoWriter) {
        m_framesPiped = m_videoWriter->framesWritten();
        m_videoWriter->requestStop();
        m_videoWriter->unblockPendingWrite();
        if (m_videoWriter->wait(3000)) delete m_videoWriter;
        m_videoWriter = nullptr;
    }

    if (m_audioPipe) {
        DisconnectNamedPipe(static_cast<HANDLE>(m_audioPipe));
    }
    if (m_videoPipe) {
        DisconnectNamedPipe(static_cast<HANDLE>(m_videoPipe));
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

    // The stages, once per run, so a workload is mechanically interpretable
    // rather than a matter of reading a status bar at the right moment. The
    // gaps between these numbers are the diagnosis: source against composed is
    // what the machine could not carry away from the capture backend, composed
    // against accepted is what this application refused to hand over, and CFR
    // DUP is what the output side invented to hold a cadence. Idle ticks are
    // timer opportunities where nothing new existed, and are not loss of any
    // kind.
    //
    // The two source figures are this run's, not the application's: they are
    // the difference from the totals taken when the run started.
    const CaptureStats sourceNow = m_sourceStats ? m_sourceStats() : CaptureStats{};
    // A run that ends mid burst still had that burst.
    closeDropBurst();

    const int piped = m_videoWriter ? m_videoWriter->framesWritten() : m_framesPiped;
    qInfo("capture stages: SOURCE RX %d  CAP DROP %d  COMPOSED %d  ENC ACCEPT %d  "
          "PIPE WRITE %d  ENC DROP %d  ENC DROP BURST MAX %.2f s  "
          "CFR DUP %d  IDLE %d",
          sourceNow.framesProduced - m_sourceStatsAtStart.framesProduced,
          sourceNow.framesDropped  - m_sourceStatsAtStart.framesDropped,
          m_composedFramesAccepted + m_composedFramesRejected,
          m_composedFramesAccepted, piped, m_composedFramesRejected,
          double(m_longestDropBurstMs) / 1000.0,
          m_cfrDuplicates, m_idleTicks);

    if (FrameProfile::enabled())
        qInfo("%s", qPrintable(FrameProfile::report()));

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
    if (m_videoWriter) {
        m_framesPiped = m_videoWriter->framesWritten();
        m_videoWriter->requestStop();
        m_videoWriter->unblockPendingWrite();
        if (m_videoWriter->wait(3000)) delete m_videoWriter;
        m_videoWriter = nullptr;
    }
    if (m_audioPipe) {
        CloseHandle(static_cast<HANDLE>(m_audioPipe));
        m_audioPipe = nullptr;
    }
    if (m_videoPipe) {
        CloseHandle(static_cast<HANDLE>(m_videoPipe));
        m_videoPipe = nullptr;
    }
    if (m_ffmpeg) {
        m_ffmpeg->deleteLater();
        m_ffmpeg = nullptr;
    }
    // Nulling the pointer does not remove the connection made in
    // onPipeConnected, and a surviving one would deliver into the next run.
    if (m_audio) disconnect(m_audio, nullptr, this, nullptr);
    m_frames    = nullptr;
    m_audio     = nullptr;
    m_pipeReady = false;
    // m_pipeAcceptor self-deletes via finished signal
    m_pipeAcceptor = nullptr;
}

bool EncoderPipeline::shouldWriteFrame(Cadence cadence, quint64 compositionSequence,
                                       quint64 lastSentSequence, bool cadenceDue) {
    // A stream owes its ingest a frame whenever its own clock says one is due,
    // and owes nothing when it does not, whatever the compositor has been
    // doing. The sequence is deliberately not consulted: repeating the latest
    // picture keeps the negotiated rate, and letting a new picture pull a
    // frame forward would put the presentation clock in the source's hands.
    if (cadence == Cadence::ConstantRate) return cadenceDue;

    // A source that does not sequence its pictures cannot say whether this one
    // is new, so the only safe reading is that it is.
    if (compositionSequence == TimedFrameSource::kUnsequenced) return true;

    // Otherwise the same sequence means the same picture, already recorded.
    return compositionSequence != lastSentSequence;
}

void EncoderPipeline::onTickVideo() {
    if (!m_running || !m_frames || !m_ffmpeg) return;
    if (m_ffmpeg->state() != QProcess::Running) return;

    if (FrameProfile::enabled()) {
        // Recorded before the early returns below, because a tick that
        // finds nothing new still says how regularly this thread is being
        // reached.
        if (m_tickClock.isValid())
            FrameProfile::record(FrameProfile::Stage::TickGap,
                                 m_tickClock.nsecsElapsed());
        m_tickClock.start();
        // What is already waiting for the transport. The one number that
        // says whether the far end is keeping up, sampled every tick rather
        // than only when a frame is rejected.
        FrameProfile::record(FrameProfile::Stage::EncoderBacklogBytes,
                             m_videoWriter ? m_videoWriter->queuedBytes() : 0);
    }

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
        // The timer only fires when this sink's clock says a frame is due, so
        // reaching here is what "due" means today.
        const quint64 seq = m_frames->compositionSequence();
        if (!shouldWriteFrame(m_cadence, seq, m_lastSentSequence, /*cadenceDue=*/true)) {
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

    // Nothing to write into yet: ffmpeg has not opened the video pipe. Not a
    // dropped frame, because the recording has not started carrying media.
    if (!m_videoWriter) {
        ++m_idleTicks;
        return;
    }

    // Do not queue a frame the transport has no room for, and decide that
    // before paying for the conversion below.
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
    if (m_videoWriter->queuedFrames() >= VideoPipeWriter::kMaxQueuedFrames) {
        ++m_composedFramesRejected;
        noteFrameRejected();
        return;
    }

    // Convert to *straight-alpha* BGRA at canvas-native resolution. We declared
    // `-pix_fmt bgra` on stdin, which ffmpeg interprets as non-premultiplied.
    // PreviewWidget composes onto Format_ARGB32_Premultiplied for fast painting,
    // so we must force a conversion to Format_ARGB32 (Qt's un-premultiplies
    // correctly) — otherwise colors with alpha < 255 darken in the recording.
    QImage out = frame;
    {
        // Un-premultiplying, and scaling when the canvas and the output
        // disagree. Both allocate a full frame and walk every pixel.
        FrameProfile::Scoped timing(FrameProfile::Stage::EncoderConvert);
        if (out.format() != rawVideoFormat())
            out = out.convertToFormat(rawVideoFormat());
        if (out.width() != MalloyCanvas::Width || out.height() != MalloyCanvas::Height)
            out = out.scaled(MalloyCanvas::Width, MalloyCanvas::Height,
                             Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }

    // Handing the frame over, not writing it. The image is implicitly shared,
    // so ownership moves to the writer without copying eight megabytes, and
    // this thread returns whatever the pipe is doing.
    //
    // ENC ACCEPT keeps its meaning: pictures this pipeline took responsibility
    // for. What that responsibility now means is acceptance into the video
    // transport rather than a QProcess write, and PIPE WRITE counts the writes
    // the transport actually completed. The gap between the two is the
    // transport's own backlog, which used to be invisible.
    {
        FrameProfile::Scoped timing(FrameProfile::Stage::EncoderWrite);
        if (!m_videoWriter->trySubmit(std::move(out))) {
            ++m_composedFramesRejected;
            noteFrameRejected();
            return;
        }
    }
    ++m_composedFramesAccepted;
    noteFrameAccepted();
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

void EncoderPipeline::onVideoPipeConnected(int primedFrames) {
    // Queued from the acceptor thread, so it can arrive after this run has
    // been torn down. Building a writer then would start a thread on a closed
    // handle and leak it, since the next run overwrites the pointer.
    if (!m_running || !m_videoPipe || m_videoWriter) return;

    // ffmpeg has opened the video pipe, so there is a reader and writing can
    // begin. The writer owns the handle from here, and starts its count from
    // the frame the accept thread already put on the wire.
    m_videoWriter = new VideoPipeWriter(m_videoPipe, primedFrames);
    m_videoWriter->start();
}

void EncoderPipeline::onPipeConnected() {
    // Same reasoning as the video side, with a sharper consequence: this slot
    // connects the audio source, and a late delivery would add a second
    // connection that survives into the next run and enqueues every PCM chunk
    // twice, which ffmpeg reads as audio running at double rate.
    if (!m_running || !m_audioPipe || m_audioWriter) return;

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
    // Redact the accumulated tail, not the chunk.
    //
    // A pipe read returns whatever bytes are available, with no line
    // atomicity, so a URL can and does arrive split across two reads. Redacting
    // each chunk on its own then finds neither half contains the destination
    // and neither contains the key, and the unredacted remainder is joined
    // afterwards and kept. Joining first and redacting the result is what makes
    // the split irrelevant.
    //
    // Only a stream URL carries a secret. A recording's destination is a file
    // path, and masking that would hide the very thing the error is about.
    constexpr int kTailCap = 4096;
    m_stderrTail += chunk;
    m_stderrTail = redactDestination(
        std::move(m_stderrTail),
        m_target.kind == Target::Kind::Rtmp ? m_target.destination : QString());
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
                                            int* encodeFps,
                                            int* cfrDuplicates) {
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
    // Frames synthesised to hold a constant output rate. Cumulative.
    static const QRegularExpression kDupRe(
        QStringLiteral(R"(dup=\s*(\d+))"));

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

    int dups = 0;
    const auto dupMatch = kDupRe.match(text);
    if (dupMatch.hasMatch()) dups = dupMatch.captured(1).toInt();
    if (cfrDuplicates) *cfrDuplicates = dups;
    return true;
}

void EncoderPipeline::parseProgressLine(QStringView line) {
    int kbps = 0, drops = 0, fps = 0, dups = 0;
    if (tryParseProgressLine(line, &kbps, &drops, &fps, &dups)) {
        // Cumulative upstream, so keep the latest rather than adding. The max
        // guards the one case assignment would not: a line reporting lower
        // than the last, which would mean the counter had restarted.
        m_cfrDuplicates = std::max(m_cfrDuplicates, dups);
        emit progress(kbps, drops, fps, m_composedFramesRejected);
    }
}

// PipeAcceptThread is a QObject defined in this .cpp file; AUTOMOC generates
// EncoderPipeline.moc to provide its meta-object code.
#include "EncoderPipeline.moc"
