#include "recording/TimelineGraphBuilder.h"
#include "model/Canvas.h"
#include "project/MediaPathPolicy.h"
#include "recording/EncoderRegistry.h"

#include <QFileInfo>
#include <QJsonObject>

#include <algorithm>
#include <cmath>

namespace {

// One clip, read out of the snapshot and validated.
struct Clip {
    int     index = 0;          // position in the timeline array, for messages
    QString label;
    QString sourcePath;
    double  start = 0.0;        // seconds on the timeline
    double  dur = 0.0;
    double  sourceIn = 0.0;     // seconds into the source
    double  speed = 1.0;
    bool    audio = false;
    int     track = 0;
    int     tx = 0, ty = 0;
    double  scale = 100.0;
    double  rotation = 0.0;
    int     opacity = 100;
    int     gainDb = 0;
    int     pan = 0;
    int     channels = 0;
};

// Ranges for values a project file supplies. They exist to keep the arithmetic
// below defined, so they are deliberately generous: a day of timeline, a
// hundredfold scale, and the decibel range a mixer offers.
constexpr double kMaxClipSeconds  = 86400.0;
constexpr double kMinScalePercent = 1.0;
constexpr double kMaxScalePercent = 10000.0;
constexpr int    kMinGainDb       = -60;
constexpr int    kMaxGainDb       = 30;

// How far past the end of its source a clip may run before it is cut and
// reported. ffprobe gives lengths to the microsecond and dur * speed picks up
// rounding, and a millisecond is well under a frame at any rate the encoder
// takes, so cutting less than that is not worth telling anyone about.
constexpr double kClampToleranceSecs = 0.001;

QString clipName(const Clip& c) {
    return c.label.isEmpty() ? QStringLiteral("clip %1").arg(c.index + 1) : c.label;
}

// ffmpeg wants plain decimals; the C locale is not guaranteed by QString::number
// with a locale-aware formatter, so pin the format explicitly.
QString num(double v) {
    return QString::number(v, 'f', 6);
}

// Seconds as a person reads them in a message.
QString secs(double v) {
    return QString::number(v, 'f', 3);
}

// atempo only accepts 0.5 to 2.0, so a larger change is a chain of them.
QStringList atempoChain(double factor) {
    QStringList terms;
    double remaining = factor;
    while (remaining > 2.0) {
        terms << QStringLiteral("atempo=2.0");
        remaining /= 2.0;
    }
    while (remaining < 0.5) {
        terms << QStringLiteral("atempo=0.5");
        remaining /= 0.5;
    }
    if (std::abs(remaining - 1.0) > 1e-6)
        terms << QStringLiteral("atempo=%1").arg(num(remaining));
    return terms;
}

}  // namespace

RenderGraph TimelineGraphBuilder::build(const QJsonArray& timeline, const OutputSettings& output,
                                        const QHash<QString, double>& sourceSeconds) {
    RenderGraph g;
    auto fail = [&g](const QString& message) {
        g.ok = false;
        g.error = message;
        return g;
    };

    if (timeline.isEmpty())
        return fail(QStringLiteral("The timeline is empty."));
    if (timeline.size() > kMaxInputs)
        return fail(QStringLiteral("This timeline has %1 clips; renders are limited to %2.")
                        .arg(timeline.size()).arg(kMaxInputs));

    QVector<Clip> clips;
    clips.reserve(timeline.size());
    for (int i = 0; i < timeline.size(); ++i) {
        const QJsonObject o = timeline.at(i).toObject();
        Clip c;
        c.index      = i;
        c.label      = o.value(QStringLiteral("label")).toString();
        c.sourcePath = o.value(QStringLiteral("sourcePath")).toString();
        c.start      = o.value(QStringLiteral("start")).toDouble();
        c.dur        = o.value(QStringLiteral("dur")).toDouble();
        c.sourceIn   = o.value(QStringLiteral("sourceIn")).toDouble();
        c.audio      = o.value(QStringLiteral("audio")).toBool();
        c.track      = o.value(QStringLiteral("track")).toInt();
        const QJsonObject xf = o.value(QStringLiteral("transform")).toObject();
        c.tx       = xf.value(QStringLiteral("x")).toInt(0);
        c.ty       = xf.value(QStringLiteral("y")).toInt(0);
        c.scale    = xf.value(QStringLiteral("scale")).toDouble(100.0);
        c.rotation = xf.value(QStringLiteral("rotation")).toDouble(0.0);
        c.opacity  = xf.value(QStringLiteral("opacity")).toInt(100);
        const QJsonObject ap = o.value(QStringLiteral("audioParams")).toObject();
        c.gainDb   = ap.value(QStringLiteral("gainDb")).toInt(0);
        c.pan      = ap.value(QStringLiteral("pan")).toInt(0);
        c.channels = ap.value(QStringLiteral("channels")).toInt(0);
        c.speed    = o.value(QStringLiteral("speed")).toObject()
                      .value(QStringLiteral("factor")).toDouble(1.0);

        // Refusals. Each one names the clip, because "the render failed" without
        // saying which clip is unhelpful on a long timeline.
        if (c.sourcePath.isEmpty())
            return fail(QStringLiteral("\"%1\" has no source media, so it cannot be rendered.")
                            .arg(clipName(c)));
        // Checked before the existence test, not after. Asking whether a UNC
        // path exists is what performs the outbound authentication, so the
        // question has to be refused rather than answered.
        if (!MediaPathPolicy::isAllowed(c.sourcePath))
            return fail(QStringLiteral("The source for \"%1\" is not a local file, "
                                       "so it will not be opened: %2")
                            .arg(clipName(c), c.sourcePath));
        if (!QFileInfo::exists(c.sourcePath))
            return fail(QStringLiteral("The source file for \"%1\" is missing: %2")
                            .arg(clipName(c), c.sourcePath));
        // Bounded at both ends, and finite. These come out of a project file
        // and are used in arithmetic that is undefined rather than merely wrong
        // for an extreme value: c.scale reaches std::lround and then a narrowing
        // to int, and c.start reaches llround. A duration bounded only below
        // also lets a render run effectively forever and fill the disk, since
        // it becomes the -t the encoder is given.
        if (!std::isfinite(c.dur) || c.dur <= 0.0 || c.dur > kMaxClipSeconds)
            return fail(QStringLiteral("\"%1\" has no usable duration.").arg(clipName(c)));
        if (!std::isfinite(c.start) || c.start < 0.0 || c.start > kMaxClipSeconds)
            return fail(QStringLiteral("\"%1\" starts outside the supported range.")
                            .arg(clipName(c)));
        if (!std::isfinite(c.sourceIn) || c.sourceIn < 0.0 || c.sourceIn > kMaxClipSeconds)
            return fail(QStringLiteral("\"%1\" starts outside its source.").arg(clipName(c)));
        if (!std::isfinite(c.scale) || c.scale < kMinScalePercent || c.scale > kMaxScalePercent)
            return fail(QStringLiteral("\"%1\" has an unsupported scale of %2%.")
                            .arg(clipName(c), num(c.scale)));
        if (c.opacity < 0 || c.opacity > 100)
            return fail(QStringLiteral("\"%1\" has an unsupported opacity of %2.")
                            .arg(clipName(c)).arg(c.opacity));
        if (c.gainDb < kMinGainDb || c.gainDb > kMaxGainDb)
            return fail(QStringLiteral("\"%1\" has an unsupported gain of %2 dB.")
                            .arg(clipName(c)).arg(c.gainDb));
        if (!std::isfinite(c.speed))
            return fail(QStringLiteral("\"%1\" has an unsupported speed.").arg(clipName(c)));
        if (c.speed < 0.1 || c.speed > 4.0)
            return fail(QStringLiteral("\"%1\" has an unsupported speed of %2x.")
                            .arg(clipName(c)).arg(num(c.speed)));
        if (std::abs(c.rotation) > 1e-6)
            return fail(QStringLiteral("Rotation is not supported yet (\"%1\").").arg(clipName(c)));
        if (c.pan != 0)
            return fail(QStringLiteral("Audio pan is not supported yet (\"%1\").").arg(clipName(c)));
        if (c.channels != 0)
            return fail(QStringLiteral("Channel mapping is not supported yet (\"%1\").")
                            .arg(clipName(c)));

        // ADR-0001, contract 1. Left alone, trim asks for source past the end
        // of the file, the clip's layer simply stops, and whatever lies beneath
        // it fills the rest of its time with nothing said. Cut it where the
        // source ends instead, and record that it was cut. Runs after the
        // bounds above, so sourceIn and speed are finite and speed is positive.
        const auto length = sourceSeconds.constFind(c.sourcePath);
        if (length != sourceSeconds.constEnd() && std::isfinite(*length) && *length > 0.0) {
            const double available = (*length - c.sourceIn) / c.speed;
            if (available < kClampToleranceSecs)
                return fail(QStringLiteral("\"%1\" starts %2 s into its source, which is only %3 s long.")
                                .arg(clipName(c), secs(c.sourceIn), secs(*length)));
            if (c.dur > available + kClampToleranceSecs) {
                g.clamped.push_back(ClampedClip{c.index, clipName(c), c.dur, available});
                c.dur = available;
            }
        }

        clips.push_back(c);
        g.durationSecs = std::max(g.durationSecs, c.start + c.dur);
    }

    if (g.durationSecs <= 0.0)
        return fail(QStringLiteral("The timeline has no duration."));

    // Lower tracks composite first, so later overlays land on top. Ties keep the
    // snapshot order, which is the order the editor lists them in.
    QVector<Clip> video, audio;
    for (const Clip& c : clips) (c.audio ? audio : video).push_back(c);
    std::stable_sort(video.begin(), video.end(),
                     [](const Clip& a, const Clip& b) { return a.track < b.track; });

    if (video.isEmpty())
        return fail(QStringLiteral("The timeline has no video clips."));

    QStringList chains;
    // Background: fixes the output geometry, frame rate and length regardless of
    // what the clips cover, so gaps render as black instead of shifting the
    // timing of everything after them.
    chains << QStringLiteral("color=c=black:s=%1x%2:r=%3:d=%4[bg]")
                  .arg(output.width).arg(output.height).arg(output.fps).arg(num(g.durationSecs));

    int inputIndex = 0;
    QStringList videoLabels;
    for (const Clip& c : video) {
        g.inputArgs << QStringLiteral("-i") << c.sourcePath;
        const double sourceEnd = c.sourceIn + c.dur * c.speed;
        const int w = std::max(2, int(std::lround(output.width * c.scale / 100.0)));
        const int h = std::max(2, int(std::lround(output.height * c.scale / 100.0)));

        QStringList terms;
        terms << QStringLiteral("trim=start=%1:end=%2").arg(num(c.sourceIn), num(sourceEnd));
        // Reset to zero, apply speed, then place the clip at its timeline start.
        terms << QStringLiteral("setpts=(PTS-STARTPTS)/%1+%2/TB").arg(num(c.speed), num(c.start));
        terms << QStringLiteral("scale=%1:%2").arg(w).arg(h);
        if (c.opacity < 100) {
            terms << QStringLiteral("format=yuva420p");
            terms << QStringLiteral("colorchannelmixer=aa=%1").arg(num(c.opacity / 100.0));
        }
        const QString label = QStringLiteral("v%1").arg(inputIndex);
        chains << QStringLiteral("[%1:v]%2[%3]").arg(inputIndex).arg(terms.join(QLatin1Char(',')), label);
        videoLabels << label;
        ++inputIndex;
    }

    // Composite every video clip onto the background in track order. The editor
    // stores a clip's position in canvas pixels (MalloyCanvas, 1920x1080)
    // whatever the output size, so it is scaled to the output here, as the size
    // above is by being a share of the output. Rounded as 64-bit: the product
    // of two ints can exceed what an int holds.
    QString last = QStringLiteral("bg");
    for (int i = 0; i < videoLabels.size(); ++i) {
        const Clip& c = video.at(i);
        const QString out = (i == videoLabels.size() - 1) ? QStringLiteral("vout")
                                                          : QStringLiteral("t%1").arg(i);
        const qint64 x = std::llround(c.tx * double(output.width) / MalloyCanvas::Width);
        const qint64 y = std::llround(c.ty * double(output.height) / MalloyCanvas::Height);
        chains << QStringLiteral("[%1][%2]overlay=%3:%4:eof_action=pass:shortest=0[%5]")
                      .arg(last, videoLabels.at(i)).arg(x).arg(y).arg(out);
        last = out;
    }

    QStringList audioLabels;
    for (const Clip& c : audio) {
        g.inputArgs << QStringLiteral("-i") << c.sourcePath;
        const double sourceEnd = c.sourceIn + c.dur * c.speed;

        QStringList terms;
        terms << QStringLiteral("atrim=start=%1:end=%2").arg(num(c.sourceIn), num(sourceEnd));
        terms << QStringLiteral("asetpts=PTS-STARTPTS");
        terms << atempoChain(c.speed);
        if (c.gainDb != 0)
            terms << QStringLiteral("volume=%1dB").arg(c.gainDb);
        // adelay places the clip on the timeline; it takes milliseconds.
        if (c.start > 0.0)
            terms << QStringLiteral("adelay=%1:all=1").arg(qint64(std::llround(c.start * 1000.0)));
        const QString label = QStringLiteral("a%1").arg(inputIndex);
        chains << QStringLiteral("[%1:a]%2[%3]").arg(inputIndex).arg(terms.join(QLatin1Char(',')), label);
        audioLabels << label;
        ++inputIndex;
    }

    if (audioLabels.size() == 1) {
        // A single stream needs no mixer; rename it so the map below is uniform.
        chains << QStringLiteral("[%1]anull[aout]").arg(audioLabels.first());
    } else if (audioLabels.size() > 1) {
        QString inputs;
        for (const QString& l : audioLabels) inputs += QStringLiteral("[%1]").arg(l);
        chains << QStringLiteral("%1amix=inputs=%2:duration=longest:normalize=0[aout]")
                      .arg(inputs).arg(audioLabels.size());
    }
    g.hasAudio = !audioLabels.isEmpty();
    g.videoClips = video.size();
    g.audioClips = audio.size();
    g.filterGraph = chains.join(QStringLiteral(";\n"));

    g.outputArgs << QStringLiteral("-map") << QStringLiteral("[vout]");
    if (g.hasAudio) g.outputArgs << QStringLiteral("-map") << QStringLiteral("[aout]");

    // Per-codec arguments come from the same registry the recording and
    // streaming paths use, so a render honours the encoder the user picked.
    if (const EncoderRegistry::Encoder* encoder = EncoderRegistry::find(output.videoCodec)) {
        g.outputArgs << encoder->buildArgs(output, EncoderRegistry::Destination::File);
    } else {
        return fail(QStringLiteral("The configured encoder is not available: %1")
                        .arg(output.videoCodec));
    }
    g.outputArgs << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
    if (g.hasAudio) {
        g.outputArgs << QStringLiteral("-c:a") << output.audioCodec
                     << QStringLiteral("-b:a") << QStringLiteral("%1k").arg(output.audioBitratekbps);
    }
    // Bound the output by the timeline, not by whichever stream runs longest.
    g.outputArgs << QStringLiteral("-t") << num(g.durationSecs);

    g.ok = true;
    return g;
}

QString TimelineGraphBuilder::describeClamps(const QVector<ClampedClip>& clamped) {
    QStringList lines;
    for (const ClampedClip& c : clamped) {
        lines << QStringLiteral("\"%1\" runs %2 s past the end of its source, so it was cut "
                                "from %3 s to %4 s.")
                     .arg(c.name, secs(c.requestedSecs - c.renderedSecs),
                          secs(c.requestedSecs), secs(c.renderedSecs));
    }
    return lines.join(QLatin1Char('\n'));
}
