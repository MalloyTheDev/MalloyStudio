#pragma once

// Detects what the machine can do, and proposes settings that suit it.
//
// The split matters: SystemProbe touches the machine, and the recommendation is
// a pure function of a SystemProfile. That is what lets the rules be tested
// against fixed hardware profiles instead of only against whatever machine the
// tests happen to run on.
//
// It proposes; it never applies. Every value comes with the reason it was
// chosen, because a recommendation the user cannot interrogate is one they
// cannot trust, and a wrong guess should be obvious rather than mysterious.

#include "recording/OutputSettings.h"

#include <QString>
#include <QStringList>
#include <QVector>

struct SystemProfile {
    struct Encoder {
        QString id;         // ffmpeg codec id
        QString display;    // human label
        bool    isHardware = false;
    };

    QVector<Encoder> encoders;       // as reported by EncoderRegistry
    int     cpuThreads = 0;
    int     monitorWidth = 0;
    int     monitorHeight = 0;
    QString microphoneName;          // empty when nothing is connected
    // Whether inputs were actually enumerated. Without this an unchecked
    // machine and one with no microphone look identical, and the user gets told
    // they have no microphone when nobody looked.
    bool    microphonesChecked = false;
    bool    hasCamera = false;
    qint64  freeDiskBytes = 0;
    QString recordingVolume;

    // Measured upload, in kilobits per second. Zero means "not measured", which
    // is treated as unknown rather than as slow.
    int uploadKbps = 0;

    bool hasHardwareEncoder() const;
    // The encoder to prefer: hardware if any is available, else the first
    // software one, else empty.
    QString preferredEncoderId() const;
};

struct Recommendation {
    // What is being proposed. Callers apply this only if the user says so.
    OutputSettings output;

    struct Note {
        QString field;   // "Resolution"
        QString value;   // "1920 x 1080"
        QString why;     // one line, in plain language
    };
    QVector<Note> notes;

    // Things the user should know that are not settings: no microphone, no
    // hardware encoder, little disk left, upload not measured.
    QStringList warnings;
};

class AudioController;

namespace SystemProbe {
// Gathers the profile from what the app already knows how to enumerate. Cheap
// enough to run on demand; does not measure bandwidth.
//
// `audio` is the live controller, used to enumerate capture devices. Passing
// nullptr leaves the microphone unchecked rather than reported as absent.
SystemProfile detect(AudioController* audio = nullptr);
}  // namespace SystemProbe

namespace SettingsRecommender {

// Pure. `current` supplies the fields the recommendation does not decide, so
// applying it never disturbs unrelated settings.
Recommendation recommend(const SystemProfile& profile, const OutputSettings& current);

// The share of measured upload a stream should use. A stream that saturates the
// link drops frames the moment anything else touches the network.
constexpr double kUploadHeadroom = 0.75;

// Streaming ceilings that keep a broadcast within what services accept.
constexpr int kMaxStreamKbps = 8000;
constexpr int kMinStreamKbps = 1500;

}  // namespace SettingsRecommender
