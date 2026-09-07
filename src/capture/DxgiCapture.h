#pragma once
#include "ICaptureSource.h"

#include <QThread>
#include <QImage>
#include <atomic>
#include <memory>

// Captures a single monitor using DXGI Desktop Duplication.
// Runs the capture loop on its own thread; emits frameReady on each new frame.
// All DXGI/D3D11 objects live entirely on the worker thread.
class DxgiCapture : public QThread {
    Q_OBJECT
public:
    explicit DxgiCapture(int adapterIndex, int outputIndex, QObject* parent = nullptr);
    ~DxgiCapture() override;

    void requestStop();

    // Whether anything downstream wants frames.
    //
    // A backend with no consumer was still doing every readback and throwing
    // the result away: with the window minimized and nothing recording, about
    // 56 a second at 3 to 4.7 ms each, a fifth of a processor core spent
    // producing frames for nobody. The desktop is still acquired and released
    // so the duplication stays healthy, but nothing is copied or counted.
    //
    // Suspension is not a stop. The session, its statistics and everything
    // downstream of it are untouched, so resuming is not a new epoch.
    void setDelivering(bool delivering) {
        m_delivering.store(delivering, std::memory_order_relaxed);
    }

    // Frames handed to the consumer but not yet taken off its event queue.
    //
    // Each frame is a full uncompressed image, about 8 MB at 1080p, delivered
    // over a queued connection. Without a bound, a consumer that falls behind
    // accumulates them at roughly 250 MB/s: that is what turned a stalled
    // compositor into a 15 GB process. The consumer decrements this when it
    // takes a frame, and the capture loop stops emitting while it is at the
    // limit, so a slow consumer costs frames rather than memory.
    //
    // Shared rather than owned so a consumer holding it cannot dangle if this
    // object is destroyed while its events are still queued.
    std::shared_ptr<std::atomic<int>> inFlightCounter() const { return m_inFlight; }

    // What this backend produced and what it lost: the SOURCE RX and CAP DROP
    // rows. Reported in the same shape as every other backend so the two
    // numbers mean the same thing whichever one is running.
    CaptureStats stats() const {
        CaptureStats out;
        out.framesProduced = m_sourceFramesProduced.load();
        out.framesDropped  = m_droppedBeforeComposition.load();
        return out;
    }

signals:
    void frameReady(QImage frame);
    void captureError(QString message);

protected:
    void run() override;

private:
    // Two lets one frame be in flight while the next is prepared, without
    // letting a backlog form. Dropping the newest keeps the consumer at most
    // one frame behind live, which for a compositor is what matters.
    static constexpr int kMaxInFlightFrames = 2;

    int m_adapterIndex;
    int m_outputIndex;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_delivering{true};
    std::shared_ptr<std::atomic<int>> m_inFlight =
        std::make_shared<std::atomic<int>>(0);
    std::atomic<int> m_sourceFramesProduced{0};
    std::atomic<int> m_droppedBeforeComposition{0};
};
