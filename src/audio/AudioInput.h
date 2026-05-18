#pragma once
#include <QString>

// Single audio input in the mixer. Stable string `id` is the persistence key
// and is the bridge to v5 (where a Source::Type::AudioInput can reference it).
struct AudioInput {
    QString id;                  // "loopback:default", "input:{guid}" — stable across runs
    QString name;                // user-facing label, e.g. "Desktop Audio"
    QString deviceId;            // WASAPI device GUID, or empty for default endpoint
    bool    loopback = true;     // true = capture playback (desktop audio), false = capture input
    float   volume   = 1.0f;     // 0.0 .. 1.5
    float   pan      = 0.0f;     // -1.0 (full L) .. 0.0 (center) .. +1.0 (full R)
    bool    muted    = false;
    bool    connected = true;    // false when the WASAPI device is invalidated

    // Runtime-only (not persisted)
    float   peakL = 0.0f;
    float   peakR = 0.0f;
};
