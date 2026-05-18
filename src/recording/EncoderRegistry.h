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
    struct Encoder {
        QString id;           // ffmpeg codec id, e.g. "h264_nvenc"
        QString display;      // human label, e.g. "NVIDIA NVENC H.264"
        bool    isHardware;   // drives CRF vs bitrate UI toggle
        // Builds the -c:v and related codec args for a given OutputSettings.
        // Does NOT include input args, scale filter, audio, or destination.
        std::function<QStringList(const OutputSettings&)> buildArgs;
    };

    // Returns the cached list of available encoders (lazy-initialised).
    static const QList<Encoder>& available();

    // Convenience: find by id. Returns nullptr if not found.
    static const Encoder* find(const QString& id);
};
