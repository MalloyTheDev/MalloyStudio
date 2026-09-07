#pragma once

#include "WgcCapture.h"

#include <QSettings>
#include <QString>

// ---------------------------------------------------------------------------
// Which backend a display source is captured with.
//
// Two exist because they are being compared, not because one is a fallback for
// the other. Desktop Duplication is asked for frames on a loop;
// Windows.Graphics.Capture announces them. Everything downstream is identical,
// which is the point: a measurement taken with one and then the other differs
// in acquisition and in nothing else.
//
// The choice is a setting rather than a build flag so that a comparison can be
// run twice on one binary, on one machine, in one sitting.
// ---------------------------------------------------------------------------
namespace CaptureBackend {

enum class Kind { Dxgi, Wgc };

inline constexpr char kSettingsKey[] = "capture/backend";

// Parse. Anything unrecognised, including an empty value, is the DXGI backend,
// because that is the one every machine can run.
inline Kind parse(const QString& value) {
    return value.compare(QStringLiteral("wgc"), Qt::CaseInsensitive) == 0 ? Kind::Wgc
                                                                         : Kind::Dxgi;
}

inline QString name(Kind kind) {
    return kind == Kind::Wgc ? QStringLiteral("wgc") : QStringLiteral("dxgi");
}

// A label for a person rather than for a settings file.
inline QString displayName(Kind kind) {
    return kind == Kind::Wgc ? QStringLiteral("Windows.Graphics.Capture")
                             : QStringLiteral("DXGI Desktop Duplication");
}

// What the user asked for, whether or not this machine can do it.
inline Kind configured() {
    return parse(QSettings().value(QLatin1String(kSettingsKey)).toString());
}

// What will actually run. A machine too old for WGC gets the DXGI backend and
// a working recording rather than an honest refusal, and the two are kept
// separate so a status line can say that the request was downgraded instead of
// quietly reporting a backend that is not the one being used.
inline Kind effective() {
    const Kind wanted = configured();
    if (wanted == Kind::Wgc && !WgcCapture::isAvailable()) return Kind::Dxgi;
    return wanted;
}

}  // namespace CaptureBackend
