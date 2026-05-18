#include "StreamingPipeline.h"
#include "model/Canvas.h"

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

    // Twitch/YouTube CBR sweet spot: keyframe every 2 s, maxrate == bitrate,
    // bufsize == 2 × bitrate. For live streams we MUST use CBR; the bitrate
    // is carried in OutputSettings.bitrateKbps.
    const int bitrate = std::max(500, s.bitrateKbps);
    const int gop     = std::max(1, s.fps * 2);    // 2-second GOP

    // Streaming-specific video encoder args. libx264 with tune=zerolatency
    // for the lowest possible buffering; CBR with maxrate/bufsize caps the
    // bandwidth precisely (Twitch terminates streams that overshoot).
    args << QStringLiteral("-c:v")     << s.videoCodec
         << QStringLiteral("-preset")  << s.preset
         << QStringLiteral("-tune")    << QStringLiteral("zerolatency")
         << QStringLiteral("-b:v")     << QStringLiteral("%1k").arg(bitrate)
         << QStringLiteral("-maxrate") << QStringLiteral("%1k").arg(bitrate)
         << QStringLiteral("-bufsize") << QStringLiteral("%1k").arg(bitrate * 2)
         << QStringLiteral("-g")       << QString::number(gop)
         << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p")
         << QStringLiteral("-c:a")     << s.audioCodec
         << QStringLiteral("-b:a")     << QStringLiteral("%1k").arg(s.audioBitratekbps)
         // RTMP muxer + destination URL. NO -shortest (streams run forever).
         << QStringLiteral("-f")       << QStringLiteral("flv")
         << target.destination;

    return args;
}
