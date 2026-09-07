#pragma once

#include <QImage>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

// ---------------------------------------------------------------------------
// ICaptureSource: what a capture backend looks like from the outside.
//
// One picture as it left a backend, before anything composed, converted or
// wrote it. The three fields are the three questions asked of every arrival:
// what was captured, which arrival it was, and when it happened.
// ---------------------------------------------------------------------------
struct CapturedFrame {
    // The captured picture, already read back to the CPU.
    //
    // A GPU-native backend would carry a surface handle here instead, and the
    // first milestone deliberately does not: keeping the readback means a
    // backend comparison changes how frames are acquired and nothing else.
    // See ICaptureSource below for why that matters.
    QImage image;

    // Which arrival from this backend this is, counted from one.
    //
    // Capture, not composition. A composition can fold several arrivals
    // together, or happen because a scene was edited while no backend produced
    // anything at all, so the two sequences answer different questions and are
    // never the same number. TimedFrameSource::compositionSequence() is the
    // other one.
    //
    // Gaps are meaningful: a sequence that jumps by more than one says frames
    // were produced and lost between the backend and here, which is what
    // CaptureStats::framesDropped counts.
    quint64 sourceSequence = 0;

    // When the backend says this frame existed, normalised to the steady clock
    // once, here, at the boundary.
    //
    // Backends time frames differently: DXGI reports a QPC present time, WGC a
    // SystemRelativeTime, a camera whatever its driver believes. Converting at
    // the boundary means exactly one place knows about any of that, and
    // everything downstream compares like with like. Nothing further along may
    // re-time a frame from its own arrival, because that would silently
    // measure queue latency and call it capture time.
    std::chrono::steady_clock::time_point capturedAt{};

    // Holds one slot in the backend's bounded handoff for as long as this
    // frame object exists, and frees it on destruction.
    //
    // The bound has to cover the whole path to the consumer, not just the
    // backend's own queue, because the memory that has to be bounded is the
    // frames waiting on a consumer's event queue: at roughly 8 MB each and
    // 250 MB/s, an unbounded path is what turned a stalled compositor into a
    // 15 GB process. Carrying the slot in the frame means the accounting
    // cannot be forgotten at a call site or lost when a queued delivery is
    // discarded undelivered, and a consumer that keeps only the image releases
    // the slot by letting the frame go.
    //
    // Held as a void token because what it counts is the backend's business
    // and nothing here needs to see it.
    std::shared_ptr<void> inFlightSlot;
};

// What a backend has done since it started, in the two numbers that make a
// recording interpretable from the source end.
struct CaptureStats {
    // SOURCE RX: frames this backend produced, whether or not anything
    // consumed them. What the hardware and the desktop actually offered.
    int framesProduced = 0;

    // CAP DROP: frames this backend produced and then lost, because the
    // consumer was already at its limit when they arrived.
    //
    // Capture-side loss, before composition. The encoder's own rejections are
    // counted separately and mean something different: one says the machine
    // could not carry frames from the backend, the other says this application
    // composed pictures it could not hand over. A single "dropped" number
    // would hide which.
    int framesDropped = 0;
};

// A source of captured frames.
//
// Arrival is announced, never polled. A backend calls the callback when a
// frame exists; nothing upstream asks it whether one does yet. That is the
// property the whole abstraction is for, because a recorder timer that polls a
// backend re-introduces the scheduling problem that composition sequencing
// exists to remove.
//
// The callback runs on the backend's own delivery thread and must do almost
// nothing: take the frame, hand it on through a bounded queue, return. Copying,
// composing, converting or writing inside it moves backpressure into the
// capture path, where it costs frames rather than memory and cannot be
// measured.
//
// Implementations own their threading. start() may fail and say so; stop()
// must be safe to call when not started, must be idempotent, and must
// guarantee no callback runs after it returns.
class ICaptureSource {
public:
    // Invoked once per produced frame, on the backend's delivery thread.
    using FrameCallback = std::function<void(CapturedFrame&&)>;

    virtual ~ICaptureSource() = default;

    // Begins capture. Returns false if the backend could not start, in which
    // case the callback is never invoked.
    virtual bool start(FrameCallback onFrame) = 0;

    // Ends capture. No callback runs after this returns.
    virtual void stop() = 0;

    // Safe to call from any thread, including while capturing.
    virtual CaptureStats stats() const = 0;
};
