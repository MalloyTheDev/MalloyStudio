#pragma once
#include <QJsonObject>
#include <QSettings>
#include <QString>

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

    static OutputSettings load() {
        QSettings s;
        OutputSettings o;
        o.width            = s.value(QStringLiteral("output/width"),            o.width           ).toInt();
        o.height           = s.value(QStringLiteral("output/height"),           o.height          ).toInt();
        o.fps              = s.value(QStringLiteral("output/fps"),              o.fps             ).toInt();
        o.videoCodec       = s.value(QStringLiteral("output/videoCodec"),       o.videoCodec      ).toString();
        o.crf              = s.value(QStringLiteral("output/crf"),              o.crf             ).toInt();
        o.preset           = s.value(QStringLiteral("output/preset"),           o.preset          ).toString();
        o.audioCodec       = s.value(QStringLiteral("output/audioCodec"),       o.audioCodec      ).toString();
        o.audioBitratekbps = s.value(QStringLiteral("output/audioBitratekbps"), o.audioBitratekbps).toInt();
        o.container        = s.value(QStringLiteral("output/container"),        o.container       ).toString();
        o.bitrateKbps         = s.value(QStringLiteral("output/bitrateKbps"),         o.bitrateKbps        ).toInt();
        o.replayBufferSeconds = s.value(QStringLiteral("output/replayBufferSeconds"), o.replayBufferSeconds).toInt();
        o.keyframeSec         = s.value(QStringLiteral("output/keyframeSec"),         o.keyframeSec        ).toInt();
        return o;
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
        s.width            = o.value(QStringLiteral("width")).toInt(s.width);
        s.height           = o.value(QStringLiteral("height")).toInt(s.height);
        s.fps              = o.value(QStringLiteral("fps")).toInt(s.fps);
        s.videoCodec       = o.value(QStringLiteral("videoCodec")).toString(s.videoCodec);
        s.crf              = o.value(QStringLiteral("crf")).toInt(s.crf);
        s.preset           = o.value(QStringLiteral("preset")).toString(s.preset);
        s.audioCodec       = o.value(QStringLiteral("audioCodec")).toString(s.audioCodec);
        s.audioBitratekbps = o.value(QStringLiteral("audioBitratekbps")).toInt(s.audioBitratekbps);
        s.container        = o.value(QStringLiteral("container")).toString(s.container);
        s.bitrateKbps      = o.value(QStringLiteral("bitrateKbps")).toInt(s.bitrateKbps);
        s.keyframeSec      = o.value(QStringLiteral("keyframeSec")).toInt(s.keyframeSec);
        return s;
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
