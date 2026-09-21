#pragma once

// Which ffmpeg this application runs, and what version it says it is.
//
// The status bar used to print a version typed into it, which drifted from the
// binary actually found on PATH. Encoder behaviour and the filters available
// differ between ffmpeg majors, so a wrong number there sends anyone debugging
// an encode down the wrong path.
//
// The same split as MachineLoad: asking the binary touches the OS and is done
// in the background, and reading its answer is a pure function that can be
// tested against the forms real builds print.

#include <QString>
#include <QStringView>

#include <functional>

class QObject;

namespace FfmpegVersion {

struct Result {
    QString path;      // the binary asked, empty when none was found
    QString version;   // empty when it could not be read
};

// The version named on the first line of `ffmpeg -version`, or an empty
// string when that line is not an ffmpeg version line.
//
// Release builds print a number and then build decorations, as in
// "ffmpeg version 8.1.1-essentials_build-www.gyan.dev Copyright ...", and only
// the number is returned. Builds from the development branch carry no release
// number ("N-118000-g1234abcd", "2024-12-19-git-494c961379-..."), and their
// build identifier is returned whole, since that is what identifies them.
QString parse(QStringView versionOutput);

// How long `ffmpeg -version` is given before it is ended. It normally answers
// in well under a second; the bound is for a binary that never does.
constexpr int kProbeTimeoutMs = 5000;

// Runs `<ffmpegPath> -version` without waiting for it and calls done once, on
// context's thread, with what it found. An empty path means no ffmpeg was
// found. A binary that fails to start, exits with an error or overruns
// timeoutMs reports its path with an empty version; one that overruns is ended
// with ProcessTree::kill, because a package manager's launcher on PATH would
// otherwise leave its child running. When there is nothing to wait for, no
// path or a binary that cannot be started, done is called before this returns.
//
// The process belongs to context, so destroying context abandons the probe.
// done can still be reached while context is being destroyed, so it must not
// assume that anything it refers to is still alive.
void probe(const QString& ffmpegPath, QObject* context,
           std::function<void(const Result&)> done,
           int timeoutMs = kProbeTimeoutMs);

}  // namespace FfmpegVersion
