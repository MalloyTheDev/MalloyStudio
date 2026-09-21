#include "capture/CaptureCallbackGate.h"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <latch>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Target {
    int calls = 0;
};

using Gate = CaptureCallbackGate<Target>;

void delayedEventAfterOwnerDestruction() {
    auto target = std::make_unique<Target>();
    auto ownerGate = std::make_shared<Gate>(target.get());
    std::weak_ptr<Gate> lifetime = ownerGate;
    std::promise<void> eventDispatched;
    std::promise<void> resumeEvent;
    auto resume = resumeEvent.get_future();
    std::atomic<int> deliveries{0};

    // Models the platform retaining a sink that has not yet entered the gate.
    std::thread event([sinkGate = ownerGate, &eventDispatched, &resume, &deliveries] {
        eventDispatched.set_value();
        resume.wait();
        sinkGate->invoke([&deliveries](Target& owner) {
            ++owner.calls;
            deliveries.fetch_add(1);
        });
    });
    eventDispatched.get_future().wait();
    ownerGate->close();
    target.reset();
    ownerGate.reset();
    const bool sinkRetainedGate = !lifetime.expired();
    resumeEvent.set_value();
    event.join();

    require(sinkRetainedGate, "A dispatched sink must retain the gate after owner destruction");
    require(deliveries.load() == 0, "A late event invoked a destroyed owner");
    require(lifetime.expired(), "The gate leaked after the last sink returned");
}

void closeDrainsAdmittedCallback() {
    Target target;
    auto gate = std::make_shared<Gate>(&target);
    std::promise<void> callbackEntered;
    std::promise<void> releaseCallback;
    auto release = releaseCallback.get_future();
    std::atomic<bool> callbackExited{false};

    std::thread event([&] {
        gate->invoke([&](Target& owner) {
            callbackEntered.set_value();
            release.wait();
            ++owner.calls;
            callbackExited.store(true);
        });
    });
    callbackEntered.get_future().wait();
    std::promise<void> stopStarted;
    auto stopped = std::async(std::launch::async, [&] {
        stopStarted.set_value();
        gate->close();
        return callbackExited.load();
    });
    stopStarted.get_future().wait();
    const bool waited = stopped.wait_for(std::chrono::milliseconds(50))
                        == std::future_status::timeout;
    releaseCallback.set_value();
    event.join();
    const bool drained = stopped.get();
    gate->invoke([](Target& owner) { ++owner.calls; });
    gate->close();

    require(waited, "Stop returned while an admitted callback was still running");
    require(drained, "Stop did not observe callback completion");
    require(target.calls == 1, "A closed gate admitted another callback");
}

void oldSessionCannotEnterRestart() {
    Target target;
    auto oldSink = std::make_shared<Gate>(&target);
    oldSink->invoke([](Target& owner) { ++owner.calls; });
    oldSink->close();
    auto newSink = std::make_shared<Gate>(&target);
    oldSink->invoke([](Target& owner) { owner.calls += 100; });
    newSink->invoke([](Target& owner) { ++owner.calls; });
    newSink->close();
    require(target.calls == 2, "An event from the retired session entered its replacement");
}

// Keeps a callback inside the gate long enough for an unserialised delivery to
// arrive while it is still running. A spin, because a sleep or a yield can last
// a whole scheduler tick on Windows, and with the gate working every hold runs
// one after another inside the executable's time limit.
void holdInsideCallback() {
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(20);
    while (std::chrono::steady_clock::now() < until) {
    }
}

void frameAndClosedSinksShareSerialization() {
    // Several sinks deliver at once, as the frame and closed callbacks of
    // separate capture threads do, and close() arrives while they are live.
    // The threads start together and keep delivering until the gate closes,
    // and every callback holds itself open, so a delivery that is not
    // serialised lands inside another rather than between two. Threads that
    // each ran a short loop to completion before the next was even created
    // never overlapped, and the test passed with the gate's mutex removed.
    constexpr int kSinks = 4;
    constexpr int kRounds = 20;
    constexpr int kDeliveriesBeforeClose = 200;

    for (int round = 0; round < kRounds; ++round) {
        Target target;
        auto gate = std::make_shared<Gate>(&target);
        std::atomic<int> inside{0};
        std::atomic<int> deliveries{0};
        std::atomic<bool> overlapped{false};
        std::atomic<bool> closeReturned{false};
        std::atomic<bool> ranAfterClose{false};
        std::atomic<bool> wrongOwner{false};
        std::atomic<bool> stop{false};
        std::latch start(kSinks);

        const auto deliver = [&](Target& owner) {
            // Compared rather than used: a close() that does not wait for
            // callbacks can clear the target while one is being admitted.
            if (&owner != &target) {
                wrongOwner.store(true);
                return;
            }
            if (inside.fetch_add(1) != 0) overlapped.store(true);
            if (closeReturned.load()) ranAfterClose.store(true);
            // Read before the hold and written after it, so an overlap also
            // loses an update, which is what it would do to a real owner.
            const int calls = owner.calls;
            holdInsideCallback();
            owner.calls = calls + 1;
            if (closeReturned.load()) ranAfterClose.store(true);
            deliveries.fetch_add(1);
            inside.fetch_sub(1);
        };

        std::vector<std::thread> sinks;
        for (int i = 0; i < kSinks; ++i) {
            sinks.emplace_back([sinkGate = gate, &start, &stop, &deliver] {
                start.arrive_and_wait();
                while (!stop.load()) sinkGate->invoke(deliver);
            });
        }

        // Closed once the sinks are delivering, and while a callback is
        // inside, so close() races one that is running and others waiting to
        // be admitted. Waiting on the delivery count alone woke this thread
        // just as a callback finished, and close() then landed in the gap
        // before the next one far more often than chance.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while ((deliveries.load() < kDeliveriesBeforeClose || inside.load() == 0)
               && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        const bool live = deliveries.load() >= kDeliveriesBeforeClose;
        gate->close();
        closeReturned.store(true);
        const int deliveredByClose = deliveries.load();
        stop.store(true);
        for (auto& sink : sinks) sink.join();

        require(live, "The sinks never started delivering");
        require(!wrongOwner.load(), "A callback was handed something other than its owner");
        require(!overlapped.load(), "Two sinks were inside the owner at the same time");
        require(!ranAfterClose.load(), "A callback was still running after close() returned");
        require(deliveries.load() == deliveredByClose,
                "A delivery completed after close() returned");
        require(target.calls == deliveredByClose, "Concurrent sinks lost an update to the owner");
    }
}

}  // namespace

int main() {
    try {
        delayedEventAfterOwnerDestruction();
        closeDrainsAdmittedCallback();
        oldSessionCannotEnterRestart();
        frameAndClosedSinksShareSerialization();
        std::cout << "Capture callback lifetime tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
