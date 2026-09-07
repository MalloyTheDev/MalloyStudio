#pragma once
#include "recording/OutputSettings.h"

#include <QList>
#include <QString>

#include <functional>

// ---------------------------------------------------------------------------
// EncoderRegistry — discovers which ffmpeg video encoders are available on
// this machine and exposes them with display names and arg-builder lambdas.
//
// Detection happens once, lazily, by spawning:
//   ffmpeg -encoders -hide_banner -loglevel error
// and scanning for known codec IDs. Results are cached for the process lifetime.
//
// Software encoders (libx264, libx265) are always listed — even when ffmpeg
// isn't on PATH the defaults are included so the dialog is never empty.
// Hardware encoders (NVENC / QSV / AMF) are appended only when ffmpeg
// reports them as available.
// ---------------------------------------------------------------------------
class EncoderRegistry {
public:
    // Where the encoded media is going, which is what decides what the rate
    // control should hold constant.
    //
    // A wire has a budget: an ingest negotiated a bitrate and wants that
    // bitrate whatever is on screen, so a stream holds the rate and lets
    // quality move. A file has no such constraint: holding quality and letting
    // the size follow the content is what a recording wants, and it is also
    // the only rate control that is indifferent to the frame rate declared on
    // the input.
    //
    // That last part is not a nicety. The rawvideo input has to declare a
    // frame rate, because that number is the timestamp resolution and anything
    // arriving closer together than it is quantised away. Any rate targeting
    // mode then divides its budget by that same declared number, so a declared
    // rate above the real arrival rate silently costs bitrate: measured on
    // hevc_nvenc asking 4500 kbps with frames arriving at 60 a second, a
    // declared 120 produced 2354 kbps and a declared 1000 produced 431. A
    // constant quality target holds steady across all of them. ffmpeg refuses
    // to let the two be separated: -r alongside a non-CFR -fps_mode is
    // rejected as contradictory.
    enum class Destination { File, Stream };

    struct Encoder {
        QString id;           // ffmpeg codec id, e.g. "h264_nvenc"
        QString display;      // human label, e.g. "NVIDIA NVENC H.264"
        bool    isHardware;   // drives CRF vs bitrate UI toggle
        // Builds the -c:v and related codec args for a given OutputSettings.
        // Does NOT include input args, scale filter, audio, or destination.
        std::function<QStringList(const OutputSettings&, Destination)> buildArgs;
        // Streaming-only -tune flag value. "" means the encoder does NOT
        // accept -tune; StreamingPipeline appends `-tune <streamingTune>`
        // when non-empty. libx264/libx265 → "zerolatency", NVENC → "ull",
        // QSV/AMF reject -tune entirely so left empty.
        QString streamingTune;
    };

    // Returns the cached list of available encoders (lazy-initialised).
    static const QList<Encoder>& available();

    // Convenience: find by id. Returns nullptr if not found.
    static const Encoder* find(const QString& id);
};
