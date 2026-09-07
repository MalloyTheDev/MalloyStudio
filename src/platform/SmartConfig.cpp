#include "platform/SmartConfig.h"

#include "audio/AudioController.h"
#include "capture/CameraCapture.h"
#include "capture/MonitorInfo.h"
#include "project/ByteSize.h"
#include "project/RecentRecordings.h"
#include "recording/EncoderRegistry.h"

#include <QDir>
#include <QObject>
#include <QStorageInfo>
#include <QThread>

#include <algorithm>

namespace {
// Below this, a recording session is at real risk of running out of room.
constexpr qint64 kLowDiskBytes = 20LL * 1024 * 1024 * 1024;

// Bitrates each shape needs to look acceptable, in kbps. These are the
// thresholds the resolution and framerate choice is derived from, rather than
// picking a resolution and hoping the bitrate suits it.
constexpr int k1080p60Kbps = 6000;
constexpr int k1080p30Kbps = 4500;
constexpr int k720p60Kbps  = 3500;
constexpr int k720p30Kbps  = 2000;

QString describeKbps(int kbps) {
    return QStringLiteral("%1 kbps").arg(kbps);
}
}  // namespace

// ---------------------------------------------------------------------------
// SystemProfile
// ---------------------------------------------------------------------------

bool SystemProfile::hasHardwareEncoder() const {
    return std::any_of(encoders.begin(), encoders.end(),
                       [](const Encoder& e) { return e.isHardware; });
}

QString SystemProfile::preferredEncoderId() const {
    for (const Encoder& e : encoders)
        if (e.isHardware) return e.id;
    return encoders.isEmpty() ? QString() : encoders.first().id;
}

// ---------------------------------------------------------------------------
// SystemProbe
// ---------------------------------------------------------------------------

SystemProfile SystemProbe::detect(AudioController* audio) {
    SystemProfile p;

    for (const EncoderRegistry::Encoder& e : EncoderRegistry::available())
        p.encoders.push_back({e.id, e.display, e.isHardware});

    p.cpuThreads = std::max(1, QThread::idealThreadCount());

    // The largest connected monitor: a recommendation should fit the display
    // the user actually captures, and the biggest one is the best guess.
    for (const MonitorInfo& m : enumerateMonitors()) {
        if (m.geometry.width() * m.geometry.height()
            > p.monitorWidth * p.monitorHeight) {
            p.monitorWidth = m.geometry.width();
            p.monitorHeight = m.geometry.height();
        }
    }

    // Cameras come from the cache, which is warmed off the UI thread; probing
    // here would block for seconds on a machine with no camera.
    p.hasCamera = !CameraCapture::cachedDevices().isEmpty();

    if (audio) {
        p.microphonesChecked = true;
        const auto devices = audio->enumerateInputDevices();
        if (!devices.isEmpty()) p.microphoneName = devices.first().second;
    }

    const QString dir = RecentRecordings::outputDir();
    const QStorageInfo storage(dir);
    if (storage.isValid() && storage.isReady()) {
        p.freeDiskBytes = storage.bytesAvailable();
        p.recordingVolume = QDir::toNativeSeparators(storage.rootPath());
    }

    return p;
}

// ---------------------------------------------------------------------------
// SettingsRecommender
// ---------------------------------------------------------------------------

Recommendation SettingsRecommender::recommend(const SystemProfile& profile,
                                              const OutputSettings& current) {
    Recommendation rec;
    rec.output = current;   // untouched fields keep whatever the user had

    // ---- Encoder ---------------------------------------------------------
    const QString encoderId = profile.preferredEncoderId();
    if (!encoderId.isEmpty()) {
        rec.output.videoCodec = encoderId;
        QString display = encoderId;
        for (const auto& e : profile.encoders)
            if (e.id == encoderId) display = e.display;

        rec.notes.push_back({QObject::tr("Encoder"), display,
            profile.hasHardwareEncoder()
                ? QObject::tr("Hardware encoding, which leaves the processor free for whatever "
                              "you are capturing.")
                : QObject::tr("No hardware encoder was found, so this uses the processor.")});
    }
    if (!profile.hasHardwareEncoder()) {
        rec.warnings << QObject::tr("No hardware encoder detected. Encoding on the processor "
                                    "competes with whatever you are capturing.");
    }

    // ---- x264 preset from core count ------------------------------------
    if (!profile.hasHardwareEncoder() && profile.cpuThreads > 0) {
        QString preset;
        QString why;
        if (profile.cpuThreads >= 16) {
            preset = QStringLiteral("fast");
            why = QObject::tr("%1 threads is enough headroom for a slower preset, which looks "
                              "better at the same bitrate.").arg(profile.cpuThreads);
        } else if (profile.cpuThreads >= 8) {
            preset = QStringLiteral("faster");
            why = QObject::tr("A middle preset for %1 threads.").arg(profile.cpuThreads);
        } else {
            preset = QStringLiteral("veryfast");
            why = QObject::tr("With %1 threads a fast preset avoids dropped frames, which cost "
                              "more than the quality it gives up.").arg(profile.cpuThreads);
        }
        rec.output.preset = preset;
        rec.notes.push_back({QObject::tr("Preset"), preset, why});
    }

    // ---- Bitrate ---------------------------------------------------------
    int bitrate = current.bitrateKbps;
    if (profile.uploadKbps > 0) {
        bitrate = int(profile.uploadKbps * kUploadHeadroom);
        bitrate = std::clamp(bitrate, kMinStreamKbps, kMaxStreamKbps);
        rec.notes.push_back({QObject::tr("Bitrate"), describeKbps(bitrate),
            QObject::tr("About %1 percent of your measured %2 upload, leaving room for "
                        "everything else using the connection.")
                .arg(int(kUploadHeadroom * 100)).arg(describeKbps(profile.uploadKbps))});
    } else {
        bitrate = std::clamp(bitrate > 0 ? bitrate : 4500, kMinStreamKbps, kMaxStreamKbps);
        rec.notes.push_back({QObject::tr("Bitrate"), describeKbps(bitrate),
            QObject::tr("Upload speed has not been measured, so this keeps your current value. "
                        "Measure it to get a figure based on your connection.")});
        rec.warnings << QObject::tr("Upload speed was not measured, so the bitrate is a "
                                    "starting point rather than a recommendation.");
    }
    rec.output.bitrateKbps = bitrate;

    // ---- Resolution and framerate, derived from the bitrate --------------
    // Chosen together and from the bitrate, because a resolution the bitrate
    // cannot feed looks worse than a smaller one that it can.
    int width = 1920, height = 1080, fps = 60;
    QString shapeWhy;
    if (bitrate >= k1080p60Kbps) {
        width = 1920; height = 1080; fps = 60;
        shapeWhy = QObject::tr("%1 comfortably feeds 1080p60.").arg(describeKbps(bitrate));
    } else if (bitrate >= k1080p30Kbps) {
        width = 1920; height = 1080; fps = 30;
        shapeWhy = QObject::tr("%1 is enough for 1080p, but not at 60 frames per second.")
                       .arg(describeKbps(bitrate));
    } else if (bitrate >= k720p60Kbps) {
        width = 1280; height = 720; fps = 60;
        shapeWhy = QObject::tr("%1 suits 720p60. Motion stays smooth, which matters more than "
                               "resolution for most content.").arg(describeKbps(bitrate));
    } else {
        width = 1280; height = 720; fps = 30;
        shapeWhy = QObject::tr("%1 is limited, so this keeps the picture clean at 720p30 rather "
                               "than blocky at a larger size.").arg(describeKbps(bitrate));
    }

    // Never propose upscaling past the display being captured.
    if (profile.monitorWidth > 0 && profile.monitorHeight > 0
        && (width > profile.monitorWidth || height > profile.monitorHeight)) {
        width = profile.monitorWidth;
        height = profile.monitorHeight;
        shapeWhy = QObject::tr("Matched to your %1 x %2 display; there is nothing to gain from "
                               "encoding larger than the source.")
                       .arg(profile.monitorWidth).arg(profile.monitorHeight);
    }

    rec.output.width = width;
    rec.output.height = height;
    rec.output.fps = fps;
    rec.notes.push_back({QObject::tr("Resolution"),
                         QStringLiteral("%1 x %2").arg(width).arg(height), shapeWhy});
    rec.notes.push_back({QObject::tr("Framerate"), QObject::tr("%1 fps").arg(fps),
                         QObject::tr("Chosen with the resolution, from the bitrate above.")});

    // ---- Audio -----------------------------------------------------------
    rec.output.audioBitratekbps = 160;
    rec.notes.push_back({QObject::tr("Audio bitrate"), describeKbps(160),
                         QObject::tr("Transparent for speech and music, and a small share of "
                                     "the total.")});
    // Only say this when inputs were actually enumerated: claiming there is no
    // microphone because nobody looked is worse than saying nothing.
    if (profile.microphonesChecked && profile.microphoneName.isEmpty()) {
        rec.warnings << QObject::tr("No microphone was detected, so only desktop audio will be "
                                    "captured.");
    }

    // ---- Keyframes -------------------------------------------------------
    rec.output.keyframeSec = 2;
    rec.notes.push_back({QObject::tr("Keyframe interval"), QObject::tr("2 s"),
                         QObject::tr("What Twitch and YouTube require; both reject longer "
                                     "than 4 seconds.")});

    // ---- Disk ------------------------------------------------------------
    if (profile.freeDiskBytes > 0 && profile.freeDiskBytes < kLowDiskBytes) {
        rec.warnings << QObject::tr("Only %1 free on %2. A long recording at this bitrate will "
                                    "fill it.")
                            .arg(formatByteSize(profile.freeDiskBytes), profile.recordingVolume);
    }

    return rec;
}
