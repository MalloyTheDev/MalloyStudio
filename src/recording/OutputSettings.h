#pragma once
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
