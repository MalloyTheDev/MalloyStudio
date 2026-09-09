#include "capture/CaptureCallbackGate.h"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
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

void frameAndClosedSinksShareSerialization() {
    Target target;
    auto gate = std::make_shared<Gate>(&target);
    std::vector<std::thread> events;
    for (int i = 0; i < 4; ++i) {
        events.emplace_back([sinkGate = gate] {
            for (int call = 0; call < 1000; ++call) {
                sinkGate->invoke([](Target& owner) { ++owner.calls; });
            }
        });
    }
    for (auto& event : events) event.join();
    gate->close();
    require(target.calls == 4000, "Concurrent sinks did not serialize owner access");
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
