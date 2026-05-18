#include "EncoderRegistry.h"

#include <QProcess>
#include <QStandardPaths>

#include <algorithm>

namespace {

// Software encoder arg builders — CRF-based quality, no CBR.
QStringList buildSoftwareArgs(const OutputSettings& s, const QString& codec) {
    return {
        QStringLiteral("-c:v"),    codec,
        QStringLiteral("-preset"), s.preset,
        QStringLiteral("-crf"),    QString::number(s.crf),
        QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
    };
}

// Hardware encoder arg builders — CBR with maxrate/bufsize.
// All HW encoders need the bitrate instead of CRF.
QStringList buildHardwareArgs(const OutputSettings& s,
                               const QString& codec,
                               const QString& preset,
                               bool           nvencStyle) {
    const int bitrate = std::max(500, s.bitrateKbps);
    if (nvencStyle) {
        // NVENC uses -preset p1..p7, -rc, -b:v
        return {
            QStringLiteral("-c:v"),    codec,
            QStringLiteral("-preset"), preset.isEmpty() ? QStringLiteral("p4") : preset,
            QStringLiteral("-rc"),     QStringLiteral("cbr"),
            QStringLiteral("-b:v"),    QStringLiteral("%1k").arg(bitrate),
            QStringLiteral("-maxrate"), QStringLiteral("%1k").arg(bitrate),
            QStringLiteral("-bufsize"), QStringLiteral("%1k").arg(bitrate * 2),
            QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
        };
    } else {
        // QSV / AMF share the -b:v / -maxrate pattern
        return {
            QStringLiteral("-c:v"),    codec,
            QStringLiteral("-b:v"),    QStringLiteral("%1k").arg(bitrate),
            QStringLiteral("-maxrate"), QStringLiteral("%1k").arg(bitrate),
            QStringLiteral("-bufsize"), QStringLiteral("%1k").arg(bitrate * 2),
            QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
        };
    }
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
    list.push_back({
        QStringLiteral("libx264"),
        QStringLiteral("libx264 (H.264, software)"),
        false,
        [](const OutputSettings& s) { return buildSoftwareArgs(s, QStringLiteral("libx264")); }
    });
    list.push_back({
        QStringLiteral("libx265"),
        QStringLiteral("libx265 (H.265/HEVC, software)"),
        false,
        [](const OutputSettings& s) { return buildSoftwareArgs(s, QStringLiteral("libx265")); }
    });

    // --- NVIDIA NVENC ---
    if (available.contains(QStringLiteral("h264_nvenc"))) {
        list.push_back({
            QStringLiteral("h264_nvenc"),
            QStringLiteral("NVIDIA NVENC H.264"),
            true,
            [](const OutputSettings& s) {
                return buildHardwareArgs(s, QStringLiteral("h264_nvenc"),
                                         QStringLiteral("p4"), true);
            }
        });
    }
    if (available.contains(QStringLiteral("hevc_nvenc"))) {
        list.push_back({
            QStringLiteral("hevc_nvenc"),
            QStringLiteral("NVIDIA NVENC H.265/HEVC"),
            true,
            [](const OutputSettings& s) {
                return buildHardwareArgs(s, QStringLiteral("hevc_nvenc"),
                                         QStringLiteral("p4"), true);
            }
        });
    }

    // --- Intel Quick Sync ---
    if (available.contains(QStringLiteral("h264_qsv"))) {
        list.push_back({
            QStringLiteral("h264_qsv"),
            QStringLiteral("Intel QSV H.264"),
            true,
            [](const OutputSettings& s) {
                return buildHardwareArgs(s, QStringLiteral("h264_qsv"), QString(), false);
            }
        });
    }
    if (available.contains(QStringLiteral("hevc_qsv"))) {
        list.push_back({
            QStringLiteral("hevc_qsv"),
            QStringLiteral("Intel QSV H.265/HEVC"),
            true,
            [](const OutputSettings& s) {
                return buildHardwareArgs(s, QStringLiteral("hevc_qsv"), QString(), false);
            }
        });
    }

    // --- AMD AMF ---
    if (available.contains(QStringLiteral("h264_amf"))) {
        list.push_back({
            QStringLiteral("h264_amf"),
            QStringLiteral("AMD AMF H.264"),
            true,
            [](const OutputSettings& s) {
                return buildHardwareArgs(s, QStringLiteral("h264_amf"), QString(), false);
            }
        });
    }
    if (available.contains(QStringLiteral("hevc_amf"))) {
        list.push_back({
            QStringLiteral("hevc_amf"),
            QStringLiteral("AMD AMF H.265/HEVC"),
            true,
            [](const OutputSettings& s) {
                return buildHardwareArgs(s, QStringLiteral("hevc_amf"), QString(), false);
            }
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
