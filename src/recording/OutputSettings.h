#pragma once
#include <QJsonObject>
#include <QSettings>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>

// App-wide encoder/output settings persisted in QSettings.
// Not per-project: user configures once and it applies to all recordings.
// Defaults match the v4 hard-coded ffmpeg invocation so existing workflows
// are unchanged until the user explicitly opens the dialog.
struct OutputSettings {
    int     width            = 1920;
    int     height           = 1080;
    int     fps              = 30;
    QString videoCodec       = QStringLiteral("libx264");
    int     crf              = 23;
    QString preset           = QStringLiteral("veryfast");
    QString audioCodec       = QStringLiteral("aac");
    int     audioBitratekbps = 192;
    QString container        = QStringLiteral("mp4");   // used for file-extension enforcement
    // CBR bitrate used by hardware encoders and the streaming pipeline.
    // libx264 file recording ignores this field (uses crf instead).
    int     bitrateKbps      = 4500;
    // Replay buffer duration in seconds (0 = disabled).
    int     replayBufferSeconds = 0;
    // Keyframe interval in seconds. 0 = "auto" (StreamingPipeline defaults to 2s).
    // MediaController plumbs StreamSettings.keyframeSec into this field before
    // starting a stream. File recording currently ignores it (libx264 chooses
    // its own GOP). Twitch/YouTube require ≤ 4s for stable playback.
    int     keyframeSec      = 0;

    // The frame rates a chooser should offer, given the one already stored.
    //
    // The stored rate is always among them. A control that cannot represent
    // the configured value is not a display problem: the settings page writes
    // back what it shows, so a 120 fps project silently became 60 the moment
    // anyone opened that page and pressed Apply for an unrelated reason. The
    // fix is not to add 120 to a fixed list, because the field is an arbitrary
    // integer everywhere else in the application, including the rate now
    // declared to ffmpeg.
    //
    // Pure so the rule can be tested without building a window.
    static QStringList frameRateChoices(int configured) {
        QList<int> rates{24, 30, 48, 50, 60, 120, 144};
        if (configured > 0 && !rates.contains(configured)) rates.append(configured);
        std::sort(rates.begin(), rates.end(), std::greater<int>());
        QStringList out;
        for (int rate : rates) out << QString::number(rate);
        return out;
    }

    // QSettings may contain strings, wide integers, or malformed values.
    // Clamp before narrowing so a corrupt value cannot wrap into a small int.
    static int integerValue(const QVariant& value, int fallback) {
        switch (value.metaType().id()) {
            case QMetaType::Int:
            case QMetaType::UInt:
            case QMetaType::LongLong:
            case QMetaType::ULongLong:
            case QMetaType::Double:
            case QMetaType::Float:
            case QMetaType::QString:
                break;
            default:
                return fallback;
        }
        bool ok = false;
        const double number = value.toDouble(&ok);
        if (!ok || !std::isfinite(number) || std::trunc(number) != number)
            return fallback;
        return static_cast<int>(std::clamp(number,
            static_cast<double>(std::numeric_limits<int>::min()),
            static_cast<double>(std::numeric_limits<int>::max())));
    }

    OutputSettings normalized() const {
        OutputSettings o = *this;
        o.width = std::clamp(o.width, 320, 7680);
        o.height = std::clamp(o.height, 180, 4320);
        // All current video encoders emit yuv420p, which needs even dimensions.
        o.width -= o.width % 2;
        o.height -= o.height % 2;
        // Preserve custom rates offered by SettingsWorkspace. The pipeline's
        // declared input rate and millisecond timer already share this ceiling.
        o.fps = std::clamp(o.fps, 1, 1000);
        o.crf = std::clamp(o.crf, 0, 51);
        o.audioBitratekbps = std::clamp(o.audioBitratekbps, 64, 320);
        o.bitrateKbps = std::clamp(o.bitrateKbps, 500, 51000);
        o.replayBufferSeconds = std::clamp(o.replayBufferSeconds, 0, 300);
        o.keyframeSec = std::clamp(o.keyframeSec, 0, 10);

        // Match EncoderRegistry's known IDs without probing hardware during a
        // settings read. Availability is still checked by the encoder pipeline.
        const QStringList codecs{
            QStringLiteral("libx264"), QStringLiteral("libx265"),
            QStringLiteral("h264_nvenc"), QStringLiteral("hevc_nvenc"),
            QStringLiteral("h264_qsv"), QStringLiteral("hevc_qsv"),
            QStringLiteral("h264_amf"), QStringLiteral("hevc_amf")
        };
        if (!codecs.contains(o.videoCodec)) o.videoCodec = QStringLiteral("libx264");

        QStringList presets;
        QString defaultPreset;
        if (o.videoCodec.endsWith(QStringLiteral("_nvenc"))) {
            presets = {QStringLiteral("p1"), QStringLiteral("p2"), QStringLiteral("p3"),
                       QStringLiteral("p4"), QStringLiteral("p5"), QStringLiteral("p6"),
                       QStringLiteral("p7")};
            defaultPreset = QStringLiteral("p4");
        } else if (o.videoCodec.endsWith(QStringLiteral("_amf"))) {
            presets = {QStringLiteral("speed"), QStringLiteral("balanced"), QStringLiteral("quality")};
            defaultPreset = QStringLiteral("balanced");
        } else {
            presets = {QStringLiteral("veryfast"), QStringLiteral("faster"),
                       QStringLiteral("fast"), QStringLiteral("medium"),
                       QStringLiteral("slow"), QStringLiteral("slower"), QStringLiteral("veryslow")};
            defaultPreset = o.videoCodec.endsWith(QStringLiteral("_qsv"))
                ? QStringLiteral("medium") : QStringLiteral("veryfast");
            if (!o.videoCodec.endsWith(QStringLiteral("_qsv")))
                presets << QStringLiteral("ultrafast") << QStringLiteral("superfast");
        }
        if (!presets.contains(o.preset)) o.preset = defaultPreset;
        const QStringList audioCodecs{QStringLiteral("aac"), QStringLiteral("libopus"), QStringLiteral("opus")};
        if (!audioCodecs.contains(o.audioCodec)) o.audioCodec = QStringLiteral("aac");
        if (o.container != QLatin1String("mp4") && o.container != QLatin1String("mkv"))
            o.container = QStringLiteral("mp4");
        return o;
    }

    static OutputSettings load() {
        QSettings s;
        return load(s);
    }

    // Explicit settings stores keep imports and tests away from app preferences.
    static OutputSettings load(QSettings& s) {
        OutputSettings o;
        o.width = integerValue(s.value(QStringLiteral("output/width")), o.width);
        o.height = integerValue(s.value(QStringLiteral("output/height")), o.height);
        o.fps = integerValue(s.value(QStringLiteral("output/fps")), o.fps);
        o.videoCodec = s.value(QStringLiteral("output/videoCodec"), o.videoCodec).toString();
        o.crf = integerValue(s.value(QStringLiteral("output/crf")), o.crf);
        o.preset = s.value(QStringLiteral("output/preset"), o.preset).toString();
        o.audioCodec = s.value(QStringLiteral("output/audioCodec"), o.audioCodec).toString();
        o.audioBitratekbps = integerValue(s.value(QStringLiteral("output/audioBitratekbps")), o.audioBitratekbps);
        o.container = s.value(QStringLiteral("output/container"), o.container).toString();
        o.bitrateKbps = integerValue(s.value(QStringLiteral("output/bitrateKbps")), o.bitrateKbps);
        o.replayBufferSeconds = integerValue(s.value(QStringLiteral("output/replayBufferSeconds")), o.replayBufferSeconds);
        o.keyframeSec = integerValue(s.value(QStringLiteral("output/keyframeSec")), o.keyframeSec);
        return o.normalized();
    }

    // JSON form, used by persisted render jobs (docs/adr/0002-render-job-contract.md).
    // Kept beside the QSettings pair so encoder settings have one vocabulary
    // across recording, streaming and rendering.
    QJsonObject toJson() const {
        QJsonObject o;
        o.insert(QStringLiteral("width"),            width);
        o.insert(QStringLiteral("height"),           height);
        o.insert(QStringLiteral("fps"),              fps);
        o.insert(QStringLiteral("videoCodec"),       videoCodec);
        o.insert(QStringLiteral("crf"),              crf);
        o.insert(QStringLiteral("preset"),           preset);
        o.insert(QStringLiteral("audioCodec"),       audioCodec);
        o.insert(QStringLiteral("audioBitratekbps"), audioBitratekbps);
        o.insert(QStringLiteral("container"),        container);
        o.insert(QStringLiteral("bitrateKbps"),      bitrateKbps);
        o.insert(QStringLiteral("keyframeSec"),      keyframeSec);
        // replayBufferSeconds is a capture concern and is deliberately not
        // carried on a render job.
        return o;
    }

    // Missing keys fall back to the struct defaults, so a job written by an
    // older build still loads with a usable encoder configuration.
    static OutputSettings fromJson(const QJsonObject& o) {
        OutputSettings s;
        s.width            = integerValue(o.value(QStringLiteral("width")).toVariant(), s.width);
        s.height           = integerValue(o.value(QStringLiteral("height")).toVariant(), s.height);
        s.fps              = integerValue(o.value(QStringLiteral("fps")).toVariant(), s.fps);
        s.videoCodec       = o.value(QStringLiteral("videoCodec")).toString(s.videoCodec);
        s.crf              = integerValue(o.value(QStringLiteral("crf")).toVariant(), s.crf);
        s.preset           = o.value(QStringLiteral("preset")).toString(s.preset);
        s.audioCodec       = o.value(QStringLiteral("audioCodec")).toString(s.audioCodec);
        s.audioBitratekbps = integerValue(o.value(QStringLiteral("audioBitratekbps")).toVariant(), s.audioBitratekbps);
        s.container        = o.value(QStringLiteral("container")).toString(s.container);
        s.bitrateKbps      = integerValue(o.value(QStringLiteral("bitrateKbps")).toVariant(), s.bitrateKbps);
        s.keyframeSec      = integerValue(o.value(QStringLiteral("keyframeSec")).toVariant(), s.keyframeSec);
        return s.normalized();
    }

    void save() const {
        QSettings s;
        s.setValue(QStringLiteral("output/width"),            width           );
        s.setValue(QStringLiteral("output/height"),           height          );
        s.setValue(QStringLiteral("output/fps"),              fps             );
        s.setValue(QStringLiteral("output/videoCodec"),       videoCodec      );
        s.setValue(QStringLiteral("output/crf"),              crf             );
        s.setValue(QStringLiteral("output/preset"),           preset          );
        s.setValue(QStringLiteral("output/audioCodec"),       audioCodec      );
        s.setValue(QStringLiteral("output/audioBitratekbps"), audioBitratekbps);
        s.setValue(QStringLiteral("output/container"),        container       );
        s.setValue(QStringLiteral("output/bitrateKbps"),         bitrateKbps        );
        s.setValue(QStringLiteral("output/replayBufferSeconds"), replayBufferSeconds);
        s.setValue(QStringLiteral("output/keyframeSec"),         keyframeSec        );
    }
};
