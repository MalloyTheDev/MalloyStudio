#include "EncoderRegistry.h"

#include <QProcess>
#include <QStandardPaths>

#include <algorithm>

namespace {

using Destination = EncoderRegistry::Destination;

// Software encoder arg builders. CRF either way, and deliberately so: it
// already holds quality constant and is indifferent to the frame rate declared
// on the input, which is the property that matters here. Whether a stream
// would be better served by a true constant bitrate from x264 is a separate
// question about streams and not about this one.
QStringList buildSoftwareArgs(const OutputSettings& s, const QString& codec) {
    return {
        QStringLiteral("-c:v"),    codec,
        QStringLiteral("-preset"), s.preset,
        QStringLiteral("-crf"),    QString::number(s.crf),
        QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
    };
}

// Hardware encoder arg builders. A stream gets the constant bitrate its ingest
// negotiated; a file gets a constant quality target, which is both what a
// recording wants and the only mode that does not quietly lose bitrate to the
// frame rate declared on the input. See EncoderRegistry::Destination.
//
// The quality number is the same one the software encoders use. NVENC's -qp
// and libx264's -crf share the 0 to 51 scale, so one setting means the same
// thing across encoders rather than needing a second one.
//
// Verified here on hevc_nvenc: constant quality held 1706 to 1888 kbps across
// declared input rates from 60 to 100000, where CBR fell from 4508 to 178.
// The QSV and AMF quality flags are the documented ones for those families and
// are not verified on this machine, which is equally true of the CBR flags
// they have always been given.
QStringList buildHardwareArgs(const OutputSettings& s,
                               const QString& codec,
                               const QString& preset,
                               bool           nvencStyle,
                               EncoderRegistry::Destination destination) {
    const int bitrate = std::max(500, s.bitrateKbps);
    const int quality = std::clamp(s.crf, 0, 51);
    const bool constantBitrate = destination == EncoderRegistry::Destination::Stream;

    if (nvencStyle) {
        // NVENC uses -preset p1..p7 and its own -rc vocabulary.
        QStringList args{
            QStringLiteral("-c:v"),    codec,
            QStringLiteral("-preset"), preset.isEmpty() ? QStringLiteral("p4") : preset,
        };
        if (constantBitrate) {
            args << QStringLiteral("-rc")      << QStringLiteral("cbr")
                 << QStringLiteral("-b:v")     << QStringLiteral("%1k").arg(bitrate)
                 << QStringLiteral("-maxrate") << QStringLiteral("%1k").arg(bitrate)
                 << QStringLiteral("-bufsize") << QStringLiteral("%1k").arg(bitrate * 2);
        } else {
            args << QStringLiteral("-rc") << QStringLiteral("constqp")
                 << QStringLiteral("-qp") << QString::number(quality);
        }
        args << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
        return args;
    }

    // QSV and AMF share the -b:v / -maxrate pattern for a rate target, and
    // differ in how they ask for a quality target.
    QStringList args{QStringLiteral("-c:v"), codec};
    if (constantBitrate) {
        args << QStringLiteral("-b:v")     << QStringLiteral("%1k").arg(bitrate)
             << QStringLiteral("-maxrate") << QStringLiteral("%1k").arg(bitrate)
             << QStringLiteral("-bufsize") << QStringLiteral("%1k").arg(bitrate * 2);
    } else if (codec.endsWith(QStringLiteral("_qsv"))) {
        args << QStringLiteral("-global_quality") << QString::number(quality);
    } else {
        args << QStringLiteral("-rc")   << QStringLiteral("cqp")
             << QStringLiteral("-qp_i") << QString::number(quality)
             << QStringLiteral("-qp_p") << QString::number(quality);
    }
    args << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
    return args;
}

// Probe ffmpeg and return the set of available encoder IDs.
QSet<QString> probeFfmpegEncoders() {
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) return {};

    QProcess p;
    p.start(ffmpeg, {
        QStringLiteral("-encoders"),
        QStringLiteral("-hide_banner"),
        QStringLiteral("-loglevel"), QStringLiteral("error"),
    });
    if (!p.waitForFinished(5000)) return {};

    // Parse stdout. Each encoder line looks like:
    //  V..... h264_nvenc           NVIDIA NVENC H.264 encoder (codec h264)
    // The codec ID is the second whitespace-delimited token on lines starting
    // with a capital letter (capability flags column).
    QSet<QString> ids;
    const QString output = QString::fromUtf8(p.readAllStandardOutput());
    for (const QString& line : output.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) continue;
        // Capability flags are a mix of letters and dots; first char is V/A/S/D
        if (!trimmed[0].isLetter() && trimmed[0] != QLatin1Char(' ')) continue;
        const QStringList tokens = trimmed.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (tokens.size() >= 2) ids.insert(tokens[1]);
    }
    return ids;
}

QList<EncoderRegistry::Encoder> buildRegistry() {
    const QSet<QString> available = probeFfmpegEncoders();

    QList<EncoderRegistry::Encoder> list;

    // --- Software (always listed) ---
    // libx264 / libx265 accept `-tune zerolatency` which disables look-ahead
    // and B-frames — required for live RTMP delivery to Twitch/YouTube.
    list.push_back({
        QStringLiteral("libx264"),
        QStringLiteral("libx264 (H.264, software)"),
        false,
        [](const OutputSettings& s, Destination) { return buildSoftwareArgs(s, QStringLiteral("libx264")); },
        QStringLiteral("zerolatency")
    });
    list.push_back({
        QStringLiteral("libx265"),
        QStringLiteral("libx265 (H.265/HEVC, software)"),
        false,
        [](const OutputSettings& s, Destination) { return buildSoftwareArgs(s, QStringLiteral("libx265")); },
        QStringLiteral("zerolatency")
    });

    // --- NVIDIA NVENC ---
    // NVENC's tune vocabulary is hq | ll | ull. "ull" (ultra-low-latency)
    // disables look-ahead and B-frames — the streaming counterpart to
    // libx264's `-tune zerolatency`. Requires ffmpeg 5.x+.
    if (available.contains(QStringLiteral("h264_nvenc"))) {
        list.push_back({
            QStringLiteral("h264_nvenc"),
            QStringLiteral("NVIDIA NVENC H.264"),
            true,
            [](const OutputSettings& s, Destination d) {
                return buildHardwareArgs(s, QStringLiteral("h264_nvenc"),
                                         QStringLiteral("p4"), true, d);
            },
            QStringLiteral("ull")
        });
    }
    if (available.contains(QStringLiteral("hevc_nvenc"))) {
        list.push_back({
            QStringLiteral("hevc_nvenc"),
            QStringLiteral("NVIDIA NVENC H.265/HEVC"),
            true,
            [](const OutputSettings& s, Destination d) {
                return buildHardwareArgs(s, QStringLiteral("hevc_nvenc"),
                                         QStringLiteral("p4"), true, d);
            },
            QStringLiteral("ull")
        });
    }

    // --- Intel Quick Sync ---
    // QSV does NOT accept `-tune`; passing it produces "Unknown option" and
    // ffmpeg exits -22. Leave streamingTune empty so StreamingPipeline omits it.
    if (available.contains(QStringLiteral("h264_qsv"))) {
        list.push_back({
            QStringLiteral("h264_qsv"),
            QStringLiteral("Intel QSV H.264"),
            true,
            [](const OutputSettings& s, Destination d) {
                return buildHardwareArgs(s, QStringLiteral("h264_qsv"), QString(), false, d);
            },
            QString()
        });
    }
    if (available.contains(QStringLiteral("hevc_qsv"))) {
        list.push_back({
            QStringLiteral("hevc_qsv"),
            QStringLiteral("Intel QSV H.265/HEVC"),
            true,
            [](const OutputSettings& s, Destination d) {
                return buildHardwareArgs(s, QStringLiteral("hevc_qsv"), QString(), false, d);
            },
            QString()
        });
    }

    // --- AMD AMF ---
    // AMF rejects -tune. Use -usage lowlatency separately if we ever need
    // that knob; for v7 the CBR rate-control alone is enough.
    if (available.contains(QStringLiteral("h264_amf"))) {
        list.push_back({
            QStringLiteral("h264_amf"),
            QStringLiteral("AMD AMF H.264"),
            true,
            [](const OutputSettings& s, Destination d) {
                return buildHardwareArgs(s, QStringLiteral("h264_amf"), QString(), false, d);
            },
            QString()
        });
    }
    if (available.contains(QStringLiteral("hevc_amf"))) {
        list.push_back({
            QStringLiteral("hevc_amf"),
            QStringLiteral("AMD AMF H.265/HEVC"),
            true,
            [](const OutputSettings& s, Destination d) {
                return buildHardwareArgs(s, QStringLiteral("hevc_amf"), QString(), false, d);
            },
            QString()
        });
    }

    return list;
}

} // namespace

const QList<EncoderRegistry::Encoder>& EncoderRegistry::available() {
    static const QList<Encoder> s_encoders = buildRegistry();
    return s_encoders;
}

const EncoderRegistry::Encoder* EncoderRegistry::find(const QString& id) {
    for (const Encoder& e : available())
        if (e.id == id) return &e;
    return nullptr;
}
