#pragma once

#include "ICaptureSource.h"

#include <QColorSpace>

#include <atomic>
#include <memory>
#include <utility>

// A lease covers preparation, every queued delivery, and any image retained by
// a consumer. The image owns the lease, so disconnecting or deleting a receiver
// releases its queued frames without a matching callback into the producer.
class CaptureFrameHandoff {
public:
    // One displayed image and one pending image keep a slow consumer close to
    // live without allowing an event queue full of uncompressed frames.
    static constexpr int kMaxInFlightFrames = 2;

    bool tryAcquire(CapturedFrame& frame) {
        frame = {};
        frame.sourceSequence = quint64(m_state->produced.fetch_add(1, std::memory_order_relaxed)) + 1;
        frame.capturedAt = std::chrono::steady_clock::now();

        int count = m_state->inFlight.load(std::memory_order_relaxed);
        while (count < kMaxInFlightFrames) {
            if (m_state->inFlight.compare_exchange_weak(
                    count, count + 1, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                frame.inFlightSlot = std::shared_ptr<void>(
                    m_state.get(), [state = m_state](void*) {
                        state->inFlight.fetch_sub(1, std::memory_order_release);
                    });
                return true;
            }
        }

        m_state->dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    CaptureStats stats() const {
        return {m_state->produced.load(std::memory_order_relaxed),
                m_state->dropped.load(std::memory_order_relaxed)};
    }

    int inFlight() const {
        return m_state->inFlight.load(std::memory_order_acquire);
    }

    // Preserve the public QImage signal while keeping CapturedFrame's lease
    // alive across its implicitly shared copies. This is a read-only wrapper:
    // a consumer that edits pixels detaches rather than changing the source.
    static QImage imageForDelivery(CapturedFrame frame) {
        if (frame.image.isNull() || !frame.inFlightSlot) return {};

        auto* retained = new CapturedFrame(std::move(frame));
        QImage image(retained->image.constBits(), retained->image.width(),
                     retained->image.height(), retained->image.bytesPerLine(),
                     retained->image.format(),
                     [](void* owner) { delete static_cast<CapturedFrame*>(owner); },
                     retained);
        image.setDevicePixelRatio(retained->image.devicePixelRatio());
        image.setColorSpace(retained->image.colorSpace());
        return image;
    }

private:
    struct State {
        std::atomic<int> inFlight{0};
        std::atomic<int> produced{0};
        std::atomic<int> dropped{0};
    };

    std::shared_ptr<State> m_state = std::make_shared<State>();
};
