#pragma once

#include <QObject>
#include <QThread>

#include <utility>

// Stops a worker thread and disposes of it, without ever destroying a QThread
// that is still running.
//
// Qt treats destroying a running QThread as fatal: the process aborts, and any
// recording or stream in progress goes with it. A worker can outlast any
// reasonable wait when it is blocked in a call that someone else services:
// PrintWindow on a window whose application has stopped responding, or a
// driver that hangs while an audio device is activated. Such a worker is cut
// loose instead. Every connection from it is dropped, so nothing it produces
// afterwards reaches anyone; it is taken from its parent, so the parent's
// destruction cannot delete it; and it deletes itself once its thread ends. If
// the thread never ends, the worker is reclaimed with the process.
//
// lastLook runs after the wait, whichever way it went, while the worker is
// still certainly alive: it is where final counters are read. The worker's
// class must provide requestStop(), callable from this thread.
//
// Returns whether the worker stopped in time and was deleted here. Either way
// the caller must forget the pointer.
template <typename Worker, typename LastLook>
bool retireWorker(Worker* worker, int timeoutMs, LastLook&& lastLook) {
    if (!worker) return true;
    worker->requestStop();
    const bool stopped = worker->wait(timeoutMs);
    std::forward<LastLook>(lastLook)(*worker);
    if (stopped) {
        delete worker;
        return true;
    }
    QObject::disconnect(worker, nullptr, nullptr, nullptr);
    worker->setParent(nullptr);
    QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    // The thread may have ended between the wait and the connection, in which
    // case finished() has already been emitted. A second deleteLater is safe.
    if (worker->isFinished()) worker->deleteLater();
    return false;
}

template <typename Worker>
bool retireWorker(Worker* worker, int timeoutMs) {
    return retireWorker(worker, timeoutMs, [](const Worker&) {});
}
