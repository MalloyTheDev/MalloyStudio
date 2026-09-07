#include "platform/MachineLoad.h"

#include <windows.h>

namespace {
// A FILETIME is a 64-bit count split across two 32-bit halves.
quint64 toTicks(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}
}  // namespace

MachineLoad::CpuSample MachineLoad::readCpu() {
    CpuSample s;
    FILETIME idle{}, kernel{}, user{};
    if (!GetSystemTimes(&idle, &kernel, &user)) return s;

    // Windows counts idle time inside the kernel total, so kernel + user is
    // already the whole interval and idle must not be added again.
    s.idleTicks  = toTicks(idle);
    s.totalTicks = toTicks(kernel) + toTicks(user);
    s.valid      = true;
    return s;
}

double MachineLoad::cpuBusyPercent(const CpuSample& prev, const CpuSample& now) {
    if (!prev.valid || !now.valid) return -1.0;

    // Counters can go backwards across a suspend or a counter reset. That says
    // nothing about load, so report that rather than a fabricated spike.
    if (now.totalTicks < prev.totalTicks || now.idleTicks < prev.idleTicks) return -1.0;

    const quint64 total = now.totalTicks - prev.totalTicks;
    if (total == 0) return -1.0;   // sampled twice within one tick

    const quint64 idle = now.idleTicks - prev.idleTicks;
    if (idle >= total) return 0.0;   // fully idle, and never negative

    const double busy = 100.0 * double(total - idle) / double(total);
    return busy > 100.0 ? 100.0 : busy;
}

double MachineLoad::memoryUsedPercent() {
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    if (!GlobalMemoryStatusEx(&m)) return -1.0;
    if (m.ullTotalPhys == 0) return -1.0;

    const double used = double(m.ullTotalPhys - m.ullAvailPhys);
    return 100.0 * used / double(m.ullTotalPhys);
}
