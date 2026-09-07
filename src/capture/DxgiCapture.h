#pragma once
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

    // Frames this backend produced, whether or not anything consumed them.
    // The SOURCE RX row: it says what the hardware and the desktop actually
    // offered, which is the only number that makes the rest interpretable.
    int sourceFramesProduced() const { return m_sourceFramesProduced.load(); }

    // Frames this backend produced and then lost because the consumer was
    // already at its limit. Capture-side loss, before composition: the
    // encoder's own rejections are counted separately and mean something
    // different. This is the CAP DROP half of the pair.
    int sourceFramesDropped() const { return m_droppedBeforeComposition.load(); }

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
    std::shared_ptr<std::atomic<int>> m_inFlight =
        std::make_shared<std::atomic<int>>(0);
    std::atomic<int> m_sourceFramesProduced{0};
    std::atomic<int> m_droppedBeforeComposition{0};
};
