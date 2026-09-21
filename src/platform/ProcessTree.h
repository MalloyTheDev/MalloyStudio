#pragma once

#include <QList>

// Ending a child process together with everything it started.
//
// Killing a process on Windows ends that process only; its own children carry
// on. That matters here because ffmpeg and ffprobe are often installed through
// a package manager that puts a small launcher on PATH, which starts the real
// program as its child. QProcess::kill() ended the launcher and left the real
// encoder or probe running unsupervised: a recording that had to be killed
// went on holding its file open and never finalised it, a cancelled render
// kept rendering, and a probe that hung kept its process for good.
namespace ProcessTree {

// Every process descended from pid, children before their own children.
// A process only counts as a child if it was created after its parent, so a
// process whose recorded parent ID has since been reused is not mistaken for
// one.
QList<quint32> descendants(quint32 pid);

// Terminates pid and all of its descendants, the descendants first. Returns
// false if any of them could not be terminated.
bool kill(quint32 pid);

// Whether a process with this ID exists and has not exited.
bool isRunning(quint32 pid);

}  // namespace ProcessTree
