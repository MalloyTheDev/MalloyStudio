#pragma once

#include <atomic>
#include <mutex>
#include <utility>

// Event sinks share this gate, rather than the owner's lifetime. Closing it
// drains admitted callbacks and makes even a late event harmless after the
// owner has been destroyed. Use a new gate for each capture session.
template <typename Target>
class CaptureCallbackGate final {
public:
    explicit CaptureCallbackGate(Target* target) : m_target(target) {}

    template <typename Callback>
    void invoke(Callback&& callback) {
        if (!m_open.load(std::memory_order_acquire)) return;

        std::lock_guard<std::mutex> guard(m_delivery);
        if (!m_open.load(std::memory_order_acquire)) return;
        std::forward<Callback>(callback)(*m_target);
    }

    // Call from the owning thread, not from a callback admitted by this gate.
    void close() {
        m_open.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> guard(m_delivery);
        m_target = nullptr;
    }

private:
    std::atomic<bool> m_open{true};
    std::mutex m_delivery;
    Target* m_target;
};
