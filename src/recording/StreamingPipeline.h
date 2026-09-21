#pragma once
#include "recording/EncoderPipeline.h"

// StreamingPipeline — RTMP push via ffmpeg's -f flv muxer.
// Overrides buildOutputArgs() to swap the file-output args for streaming
// args: the stream rate control EncoderRegistry builds (CBR on hardware,
// capped CRF in software), a low-latency tune and a fixed keyframe interval.
//
// Stream-specific config (bitrate, keyframe interval) is carried in the
// OutputSettings struct inside Target::output — MediaController fills them
// in from StreamSettings before calling start().
class StreamingPipeline : public EncoderPipeline {
    Q_OBJECT
public:
    explicit StreamingPipeline(QObject* parent = nullptr) : EncoderPipeline(parent) {}

protected:
    QStringList buildOutputArgs(const Target& target) const override;
};
