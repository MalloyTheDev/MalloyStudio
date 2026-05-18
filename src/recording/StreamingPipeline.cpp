#include "StreamingPipeline.h"
#include "model/Canvas.h"
#include "recording/EncoderRegistry.h"

#include <algorithm>

// Builds RTMP-specific output/codec/muxer args only.
// The base class prepends the common input section (rawvideo stdin + audio
// named pipe) before these are appended — no input args here.
QStringList StreamingPipeline::buildOutputArgs(const Target& target) const {
    const OutputSettings& s = target.output;
    QStringList args;

    // Scale filter when output differs from canvas native.
    if (s.width != MalloyCanvas::Width || s.height != MalloyCanvas::Height) {
        args << QStringLiteral("-vf")
             << QStringLiteral("scale=%1:%2").arg(s.width).arg(s.height);
    }

    // --- Per-encoder codec args via EncoderRegistry ---
    //
    // PRE-v7 BUG: this block used to hardcode libx264-only flags
    // (`-c:v <s.videoCodec> -preset <s.preset> -tune zerolatency -b:v ...`).
    // For any hardware encoder (NVENC / QSV / AMF) ffmpeg rejected the libx264
    // preset name and exited -22 on Start Stream. RecorderPipeline already
    // delegated to EncoderRegistry as of v6 — the streaming path missed that
    // fix. We now consult the registry exactly the same way.
    const EncoderRegistry::Encoder* enc = EncoderRegistry::find(s.videoCodec);
    if (enc) {
        args << enc->buildArgs(s);
    } else {
        // Unknown codec id — libx264-style fallback (matches the RecorderPipeline
        // fallback in EncoderPipeline::buildOutputArgs). Should never fire via
        // the UI because the combo is populated from the registry, but guards
        // against a project file or QSettings entry from a different build.
        const int bitrate = std::max(500, s.bitrateKbps);
        args << QStringLiteral("-c:v")     << s.videoCodec
             << QStringLiteral("-preset")  << s.preset
             << QStringLiteral("-b:v")     << QStringLiteral("%1k").arg(bitrate)
             << QStringLiteral("-maxrate") << QStringLiteral("%1k").arg(bitrate)
             << QStringLiteral("-bufsize") << QStringLiteral("%1k").arg(bitrate * 2)
             << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
    }

    // --- Streaming-only `-tune` flag, per-encoder ---
    // libx264/libx265 → "zerolatency", NVENC → "ull", QSV/AMF → "" (skipped).
    // Adding the wrong tune name was the other half of the -22 bug.
    if (enc && !enc->streamingTune.isEmpty()) {
        args << QStringLiteral("-tune") << enc->streamingTune;
    }

    // --- Forced fixed GOP for live delivery ---
    // Twitch terminates streams that don't keyframe at least every 4 s, and
    // YouTube prefers ≤ 2 s. Software encoders' buildSoftwareArgs and NVENC's
    // buildHardwareArgs don't emit `-g`, so we append it here. ffmpeg's
    // last `-g` wins so this safely overrides anything the registry produced.
    //
    // keyframeSec is plumbed from StreamSettings via MediaController; 0 means
    // "auto / use the streaming default of 2 s".
    const int keyframeSec = s.keyframeSec > 0 ? s.keyframeSec : 2;
    const int gop = std::max(1, s.fps * keyframeSec);
    args << QStringLiteral("-g") << QString::number(gop);

    // --- Audio + RTMP muxer ---
    args << QStringLiteral("-c:a") << s.audioCodec
         << QStringLiteral("-b:a") << QStringLiteral("%1k").arg(s.audioBitratekbps)
         // RTMP muxer + destination URL. NO -shortest (streams run forever).
         << QStringLiteral("-f")   << QStringLiteral("flv")
         << target.destination;

    return args;
}
