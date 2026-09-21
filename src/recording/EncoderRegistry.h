#pragma once
#include "recording/OutputSettings.h"

#include <QList>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

// ---------------------------------------------------------------------------
// EncoderRegistry — discovers which ffmpeg video encoders are available on
// this machine and exposes them with display names and arg-builder lambdas.
//
// What ffmpeg was built with is not what this machine can run: the common
// Windows builds include NVENC, Quick Sync and AMF whatever GPU is installed,
// and offering one the machine lacks meant a recording or stream that failed
// the moment it started. So a hardware encoder is offered only once a short
// trial encode with it has succeeded here. startHardwareCheck() runs that
// check once per session, on a worker thread, and each trial has a time
// limit; until it reports, no hardware encoder is offered.
//
// Software encoders (libx264, libx265) are always listed — even when ffmpeg
// isn't on PATH the defaults are included so the dialog is never empty.
// find() knows every encoder this application can drive, verified or not,
// so a codec saved earlier, or chosen before the check reported, still gets
// its own arguments rather than libx264's.
// ---------------------------------------------------------------------------
class EncoderRegistryNotifier;

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

    // Whether this machine can run an encoder. Software encoders always
    // work; a hardware one is Unchecked until the check has tried it.
    enum class Support { Unchecked, Works, Unavailable };

    // How the check reaches ffmpeg. Tests replace it, so that no test runs a
    // trial encode on the hardware of whatever machine it happens to be on.
    struct Probe {
        // The encoder ids the ffmpeg build includes; empty without ffmpeg.
        std::function<QSet<QString>()> builtIn;
        // Whether a short trial encode with this encoder succeeded. It runs
        // on the check's worker thread and must bound its own time.
        std::function<bool(const QString& id)> trial;
    };

    // The check as shipped: `program -encoders`, then a trial encode of a
    // fraction of a second of blank test video per candidate. Each run is
    // stopped, with everything it started, once it outlasts timeoutMs.
    // `leadingArgs` go before ffmpeg's own, so a test can stand another
    // program in for ffmpeg.
    static Probe ffmpegProbe(const QString& program, const QStringList& leadingArgs = {},
                             int timeoutMs = 10000);
    // ffmpeg from PATH, found the way the recording pipelines find it.
    static Probe systemProbe();

    // Runs the check on a worker thread and returns at once. Once per
    // session: later calls do nothing. Call from the GUI thread; the results
    // are applied there, and then notifier() emits hardwareChecked().
    static void startHardwareCheck(Probe probe);
    static EncoderRegistryNotifier* notifier();

    // The encoders to offer and to recommend: the software ones, and each
    // hardware one that has passed its trial on this machine. GUI thread. The
    // list is replaced when the check reports, so do not hold on to it
    // across the event loop.
    static const QList<Encoder>& available();

    // Any encoder this application can drive, whether or not this machine
    // can run it. Returns nullptr for an id it does not know. Safe from any
    // thread: the entries never change.
    static const Encoder* find(const QString& id);

    static Support support(const QString& id);

    // An encoder's name, saying so when this machine cannot run it or it has
    // not been checked yet.
    static QString label(const QString& id);

    // What a picker should list, as (id, label): the available encoders,
    // and `keep` after them when it is not one of those. A saved choice that
    // is not offered stays visible, marked, rather than being replaced by
    // the first entry and written back as a different encoder.
    static QList<QPair<QString, QString>> choices(const QString& keep);

    // Forgets the check and its results, so a test can run it again. A check
    // still running when this is called reports into nothing.
    static void resetForTesting();
};

// Emits on the GUI thread once the hardware check has been applied.
class EncoderRegistryNotifier : public QObject {
    Q_OBJECT
signals:
    void hardwareChecked();
};
