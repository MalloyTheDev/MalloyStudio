#pragma once
#include "recording/EncoderPipeline.h"

// StreamingPipeline — RTMP push via ffmpeg's -f flv muxer.
// Overrides buildFfmpegArgs() to swap the file-output args for streaming
// args (CBR, low-latency tune, fixed keyframe interval, no -shortest).
//
// Stream-specific config (bitrate, keyframe interval) is carried in the
// OutputSettings struct inside Target::output — MediaController fills them
// in from StreamSettings before calling start().
class StreamingPipeline final : public EncoderPipeline {
    Q_OBJECT
public:
    explicit StreamingPipeline(QObject* parent = nullptr) : EncoderPipeline(parent) {}

protected:
    QStringList buildOutputArgs(const Target& target) const override;
};
