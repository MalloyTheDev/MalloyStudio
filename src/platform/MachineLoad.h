#pragma once

// Machine load, measured rather than guessed.
//
// Named MachineLoad rather than the more obvious SystemLoad because winnt.h
// already defines SystemLoad as an enumerator, and the collision is a compile
// error the moment windows.h is in the same translation unit.
//
// The split is the same one SmartConfig uses and for the same reason: reading
// the counters touches the OS, but turning two readings into a percentage is
// arithmetic, and arithmetic can be tested. Every function here reports that it
// does not know rather than returning a plausible number, because the whole
// point of a load indicator is that the user can believe it.

#include <QtGlobal>

namespace MachineLoad {

// A raw reading of the processor's cumulative tick counters. Absolute values
// are meaningless on their own; only the difference between two readings says
// anything.
struct CpuSample {
    quint64 idleTicks  = 0;
    quint64 totalTicks = 0;   // idle + kernel + user
    bool    valid      = false;
};

// Reads the machine's current counters. `valid` is false when the platform
// call fails.
CpuSample readCpu();

// Pure. Busy time between two readings, 0 to 100.
//
// Returns -1 when the pair says nothing: either sample invalid, no time
// elapsed between them, or counters that moved backwards (which happens across
// a suspend, and must not be reported as a spike).
double cpuBusyPercent(const CpuSample& prev, const CpuSample& now);

// Physical memory in use, 0 to 100, or -1 when it cannot be read.
double memoryUsedPercent();

}  // namespace MachineLoad
