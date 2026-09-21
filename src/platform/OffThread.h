#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QPromise>
#include <QThreadPool>

#include <memory>
#include <type_traits>
#include <utility>

// Running a blocking file system call away from the GUI thread.
//
// Listing a folder, or asking a volume how much space is free, normally takes
// microseconds. On a network share whose server has gone away (the VPN is
// down, the NAS is off) the same call waits for the redirector to give up,
// often tens of seconds, and made on the GUI thread it freezes the window for
// all of that time.
namespace OffThread {

// The threads the work runs on. Created once and deliberately never destroyed:
// the global pool is waited for when the application object is destroyed, so a
// thread still blocked on a vanished share would hold the process open after
// its window had closed. These threads are simply ended with the process.
inline QThreadPool* pool() {
    static QThreadPool* const threads = [] {
        auto* p = new QThreadPool;
        // A stalled share holds one thread for as long as it stalls. Callers
        // keep at most one call per folder or volume outstanding, so this is
        // room for several at once; anything beyond it waits its turn.
        p->setMaxThreadCount(8);
        return p;
    }();
    return threads;
}

// Runs `work` on a pool thread and then `done(result)` on `context`'s thread,
// by a queued call. If `context` is destroyed first, the result is dropped and
// `done` is never called. `work` must not touch `context` or anything it owns:
// it takes what it needs by value.
template <typename Work, typename Done>
void run(QObject* context, Work work, Done done) {
    using Result = std::invoke_result_t<Work&>;
    auto promise = std::make_shared<QPromise<Result>>();
    // Owned by the context, so the answer dies with it.
    auto* watcher = new QFutureWatcher<Result>(context);
    QObject::connect(watcher, &QFutureWatcherBase::finished, watcher,
                     [watcher, done = std::move(done)]() mutable {
        watcher->deleteLater();
        if (watcher->future().resultCount() > 0) done(watcher->result());
    });
    watcher->setFuture(promise->future());
    pool()->start([promise, work = std::move(work)]() mutable {
        promise->start();
        promise->addResult(work());
        promise->finish();
    });
}

} // namespace OffThread
