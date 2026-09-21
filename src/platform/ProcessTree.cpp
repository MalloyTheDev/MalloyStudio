#include "platform/ProcessTree.h"

#include <QHash>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

namespace {

// Creation time as a 64-bit count, or 0 when the process cannot be queried.
quint64 creationTime(quint32 pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0;
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool ok = GetProcessTimes(h, &created, &exited, &kernel, &user);
    CloseHandle(h);
    if (!ok) return 0;
    return (quint64(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

}  // namespace

QList<quint32> ProcessTree::descendants(quint32 pid) {
    QMultiHash<quint32, quint32> children;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return {};
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snap, &entry); more; more = Process32NextW(snap, &entry)) {
        if (entry.th32ProcessID != entry.th32ParentProcessID)
            children.insert(entry.th32ParentProcessID, entry.th32ProcessID);
    }
    CloseHandle(snap);

    QList<quint32> found;
    QList<quint32> frontier{pid};
    while (!frontier.isEmpty()) {
        const quint32 parent = frontier.takeFirst();
        const quint64 parentBorn = creationTime(parent);
        for (const quint32 child : children.values(parent)) {
            if (found.contains(child) || child == pid) continue;
            // A parent ID is only a number, and numbers are reused: a process
            // older than its supposed parent cannot be its child.
            const quint64 childBorn = creationTime(child);
            if (parentBorn && childBorn && childBorn < parentBorn) continue;
            found.append(child);
            frontier.append(child);
        }
    }
    return found;
}

namespace {
// Ends one process. A process that has already exited counts as ended: a
// launcher whose child was just terminated usually exits on its own first.
bool terminateOne(quint32 pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return GetLastError() == ERROR_INVALID_PARAMETER;   // no such process any more
    bool ended = TerminateProcess(h, 1);
    if (!ended) {
        DWORD code = 0;
        ended = GetExitCodeProcess(h, &code) && code != STILL_ACTIVE;
    }
    CloseHandle(h);
    return ended;
}
}  // namespace

bool ProcessTree::kill(quint32 pid) {
    bool all = true;
    const QList<quint32> doomed = descendants(pid);
    // Deepest first, so a launcher is not left waiting on a child that is
    // still running, and the root last.
    for (auto it = doomed.crbegin(); it != doomed.crend(); ++it)
        all = terminateOne(*it) && all;
    return terminateOne(pid) && all;
}

bool ProcessTree::isRunning(quint32 pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    const bool running = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return running;
}
