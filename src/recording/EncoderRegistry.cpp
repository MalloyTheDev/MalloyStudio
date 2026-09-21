#include "EncoderRegistry.h"

#include "platform/ProcessTree.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QHash>
#include <QMutex>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>

#include <algorithm>
#include <atomic>

namespace {

using Destination = EncoderRegistry::Destination;

// Software encoder arg builders. CRF either way, and deliberately so: it
// holds quality constant and does not divide a bit budget by the frame rate
// declared on the input, which is the property that matters here.
//
// A stream is CRF with a ceiling. Uncapped, a high-motion 1080p60 scene ran
// well past the bitrate the user set and past what an uplink or an ingest
// accepts, and changing the setting did nothing: the stream bitrate never
// reached a software encoder. The VBV limits below cap the peak at that
// bitrate while CRF still decides the quality under it.
QStringList buildSoftwareArgs(const OutputSettings& s, const QString& codec,
                              Destination destination) {
    QStringList args{
        QStringLiteral("-c:v"),    codec,
        QStringLiteral("-preset"), s.preset,
        QStringLiteral("-crf"),    QString::number(s.crf),
    };
    if (destination == Destination::Stream) {
        const int bitrate = std::max(500, s.bitrateKbps);
        args << QStringLiteral("-maxrate") << QStringLiteral("%1k").arg(bitrate)
             << QStringLiteral("-bufsize") << QStringLiteral("%1k").arg(bitrate * 2);
    }
    args << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
    return args;
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

// Runs a program to completion and says whether it exited cleanly with
// status 0. One still running when timeoutMs is up is ended together with
// everything it started, since the ffmpeg on PATH is often a package
// manager's launcher whose real ffmpeg would otherwise carry on (see
// ProcessTree). Standard error is discarded rather than left in a pipe that
// nobody reads and that could fill and stall the process.
bool runBounded(const QString& program, const QStringList& args, int timeoutMs,
                QByteArray* standardOutput = nullptr) {
    if (program.isEmpty()) return false;
    QProcess p;
    if (!standardOutput) p.setStandardOutputFile(QProcess::nullDevice());
    p.setStandardErrorFile(QProcess::nullDevice());
    const QDeadlineTimer deadline(timeoutMs);
    p.start(program, args);
    const bool finished = p.waitForStarted(int(deadline.remainingTime()))
                          && p.waitForFinished(int(deadline.remainingTime()));
    if (!finished) {
        if (p.state() != QProcess::NotRunning) {
            ProcessTree::kill(quint32(p.processId()));
            p.waitForFinished(1000);
        }
        return false;
    }
    if (standardOutput) *standardOutput = p.readAllStandardOutput();
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

// The encoder ids in `ffmpeg -encoders` output.
QSet<QString> parseEncoderList(const QByteArray& listing) {
    // Parse stdout. Each encoder line looks like:
    //  V..... h264_nvenc           NVIDIA NVENC H.264 encoder (codec h264)
    // The codec ID is the second whitespace-delimited token on lines starting
    // with a capital letter (capability flags column).
    QSet<QString> ids;
    const QString output = QString::fromUtf8(listing);
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

// Every encoder this application knows how to drive. Whether this machine
// can run the hardware ones is a separate question, answered by the check.
QList<EncoderRegistry::Encoder> buildCatalog() {
    QList<EncoderRegistry::Encoder> list;

    // --- Software (always listed) ---
    // libx264 / libx265 accept `-tune zerolatency` which disables look-ahead
    // and B-frames — required for live RTMP delivery to Twitch/YouTube.
    list.push_back({
        QStringLiteral("libx264"),
        QStringLiteral("libx264 (H.264, software)"),
        false,
        [](const OutputSettings& s, Destination d) { return buildSoftwareArgs(s, QStringLiteral("libx264"), d); },
        QStringLiteral("zerolatency")
    });
    list.push_back({
        QStringLiteral("libx265"),
        QStringLiteral("libx265 (H.265/HEVC, software)"),
        false,
        [](const OutputSettings& s, Destination d) { return buildSoftwareArgs(s, QStringLiteral("libx265"), d); },
        QStringLiteral("zerolatency")
    });

    // --- NVIDIA NVENC ---
    // NVENC's tune vocabulary is hq | ll | ull. "ull" (ultra-low-latency)
    // disables look-ahead and B-frames — the streaming counterpart to
    // libx264's `-tune zerolatency`. Requires ffmpeg 5.x+.
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

    // --- Intel Quick Sync ---
    // QSV does NOT accept `-tune`; passing it produces "Unknown option" and
    // ffmpeg exits -22. Leave streamingTune empty so StreamingPipeline omits it.
    list.push_back({
        QStringLiteral("h264_qsv"),
        QStringLiteral("Intel QSV H.264"),
        true,
        [](const OutputSettings& s, Destination d) {
            return buildHardwareArgs(s, QStringLiteral("h264_qsv"), QString(), false, d);
        },
        QString()
    });
    list.push_back({
        QStringLiteral("hevc_qsv"),
        QStringLiteral("Intel QSV H.265/HEVC"),
        true,
        [](const OutputSettings& s, Destination d) {
            return buildHardwareArgs(s, QStringLiteral("hevc_qsv"), QString(), false, d);
        },
        QString()
    });

    // --- AMD AMF ---
    // AMF rejects -tune. Use -usage lowlatency separately if we ever need
    // that knob; for v7 the CBR rate-control alone is enough.
    list.push_back({
        QStringLiteral("h264_amf"),
        QStringLiteral("AMD AMF H.264"),
        true,
        [](const OutputSettings& s, Destination d) {
            return buildHardwareArgs(s, QStringLiteral("h264_amf"), QString(), false, d);
        },
        QString()
    });
    list.push_back({
        QStringLiteral("hevc_amf"),
        QStringLiteral("AMD AMF H.265/HEVC"),
        true,
        [](const OutputSettings& s, Destination d) {
            return buildHardwareArgs(s, QStringLiteral("hevc_amf"), QString(), false, d);
        },
        QString()
    });

    return list;
}

const QList<EncoderRegistry::Encoder>& catalog() {
    static const QList<EncoderRegistry::Encoder> encoders = buildCatalog();
    return encoders;
}

using Support = EncoderRegistry::Support;

// What the check has found, and the list offered from it.
struct CheckState {
    // Written on the GUI thread when a check reports; support() may be asked
    // from anywhere.
    QMutex mutex;
    QHash<QString, Support> support;

    // GUI thread only.
    QList<EncoderRegistry::Encoder> offered;
    bool started = false;
    // Raised by resetForTesting, so that a check started before it cannot
    // report into the state that came after.
    int generation = 0;
};

CheckState& checkState() {
    static CheckState state;
    return state;
}

// Set when the application starts shutting down, so a check finishing during
// teardown does not post into an event loop that is going away.
std::atomic<bool>& shuttingDown() {
    static std::atomic<bool> flag{false};
    return flag;
}

Support supportOf(const EncoderRegistry::Encoder& e) {
    if (!e.isHardware) return Support::Works;
    CheckState& state = checkState();
    QMutexLocker lock(&state.mutex);
    return state.support.value(e.id, Support::Unchecked);
}

QList<EncoderRegistry::Encoder> offeredNow() {
    QList<EncoderRegistry::Encoder> list;
    for (const EncoderRegistry::Encoder& e : catalog())
        if (supportOf(e) == Support::Works) list.push_back(e);
    return list;
}

// Runs on the check's worker thread and touches nothing shared: the probe
// is its own copy, and the results travel back by value.
QHash<QString, Support> runCheck(const EncoderRegistry::Probe& probe,
                                 const QStringList& candidates) {
    QHash<QString, Support> results;
    const QSet<QString> builtIn = probe.builtIn ? probe.builtIn() : QSet<QString>{};
    // A family's H.264 encoder comes first and is the most widely supported.
    // When it fails, the GPU or the driver that family needs is not there,
    // and its HEVC encoder is not tried.
    QSet<QString> failedFamilies;
    for (const QString& id : candidates) {
        const QString family = id.section(QLatin1Char('_'), 1);
        if (!builtIn.contains(id) || failedFamilies.contains(family)) {
            results.insert(id, Support::Unavailable);
            continue;
        }
        const bool works = probe.trial && probe.trial(id);
        results.insert(id, works ? Support::Works : Support::Unavailable);
        if (!works) failedFamilies.insert(family);
    }
    return results;
}

void applyCheck(const QHash<QString, Support>& results, int generation) {
    CheckState& state = checkState();
    if (generation != state.generation) return;
    {
        QMutexLocker lock(&state.mutex);
        state.support = results;
    }
    state.offered = offeredNow();
    emit EncoderRegistry::notifier()->hardwareChecked();
}

} // namespace

EncoderRegistry::Probe EncoderRegistry::ffmpegProbe(const QString& program,
                                                    const QStringList& leadingArgs,
                                                    int timeoutMs) {
    Probe probe;
    probe.builtIn = [program, leadingArgs, timeoutMs] {
        QByteArray listing;
        if (!runBounded(program,
                        leadingArgs + QStringList{
                            QStringLiteral("-encoders"),
                            QStringLiteral("-hide_banner"),
                            QStringLiteral("-loglevel"), QStringLiteral("error"),
                        },
                        timeoutMs, &listing)) {
            return QSet<QString>{};
        }
        return parseEncoderList(listing);
    };
    // A fifth of a second of blank test video, encoded and thrown away. An
    // encoder whose hardware or driver is missing fails as it opens, which is
    // exactly what a recording would run into.
    probe.trial = [program, leadingArgs, timeoutMs](const QString& id) {
        return runBounded(program,
                          leadingArgs + QStringList{
                              QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"),
                              QStringLiteral("-loglevel"), QStringLiteral("error"),
                              QStringLiteral("-f"), QStringLiteral("lavfi"),
                              QStringLiteral("-i"), QStringLiteral("color=c=black:s=256x144:r=30:d=0.2"),
                              QStringLiteral("-c:v"), id,
                              QStringLiteral("-f"), QStringLiteral("null"), QStringLiteral("-"),
                          },
                          timeoutMs);
    };
    return probe;
}

EncoderRegistry::Probe EncoderRegistry::systemProbe() {
    return ffmpegProbe(QStandardPaths::findExecutable(QStringLiteral("ffmpeg")));
}

EncoderRegistryNotifier* EncoderRegistry::notifier() {
    // Leaked on purpose: a check can report after everything else has gone,
    // and it must find this still there.
    static EncoderRegistryNotifier* hub = new EncoderRegistryNotifier;
    return hub;
}

void EncoderRegistry::startHardwareCheck(Probe probe) {
    CheckState& state = checkState();
    if (state.started) return;
    state.started = true;

    // Created here, on the GUI thread, which is where the results are applied.
    EncoderRegistryNotifier* hub = notifier();
    static bool hookedShutdown = false;
    if (!hookedShutdown && qApp) {
        hookedShutdown = true;
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, hub, [] { shuttingDown() = true; });
    }

    QStringList candidates;
    for (const Encoder& e : catalog())
        if (e.isHardware) candidates << e.id;
    const int generation = state.generation;

    // Each trial starts a process and waits for it, which can take seconds
    // when a driver is slow to answer; none of that may happen on the GUI
    // thread. A Qt thread rather than a bare one, so QProcess has the event
    // dispatcher it expects.
    QThread* worker = QThread::create([probe = std::move(probe), candidates, generation] {
        const QHash<QString, Support> results = runCheck(probe, candidates);
        if (shuttingDown()) return;
        QMetaObject::invokeMethod(notifier(), [results, generation] {
            applyCheck(results, generation);
        }, Qt::QueuedConnection);
    });
    QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start(QThread::LowPriority);
}

const QList<EncoderRegistry::Encoder>& EncoderRegistry::available() {
    CheckState& state = checkState();
    // Software encoders at least, so a picker is never empty.
    if (state.offered.isEmpty()) state.offered = offeredNow();
    return state.offered;
}

const EncoderRegistry::Encoder* EncoderRegistry::find(const QString& id) {
    for (const Encoder& e : catalog())
        if (e.id == id) return &e;
    return nullptr;
}

EncoderRegistry::Support EncoderRegistry::support(const QString& id) {
    const Encoder* encoder = find(id);
    return encoder ? supportOf(*encoder) : Support::Unavailable;
}

QString EncoderRegistry::label(const QString& id) {
    const Encoder* encoder = find(id);
    if (!encoder) return QObject::tr("%1 (unavailable)").arg(id);
    switch (supportOf(*encoder)) {
        case Support::Works:
            return encoder->display;
        case Support::Unchecked:
            return QObject::tr("%1 (not checked yet)").arg(encoder->display);
        case Support::Unavailable:
            return QObject::tr("%1 (not available on this machine)").arg(encoder->display);
    }
    return encoder->display;
}

QList<QPair<QString, QString>> EncoderRegistry::choices(const QString& keep) {
    QList<QPair<QString, QString>> entries;
    bool listed = keep.isEmpty();
    for (const Encoder& e : available()) {
        entries.append({e.id, e.display});
        listed = listed || e.id == keep;
    }
    if (!listed) entries.append({keep, label(keep)});
    return entries;
}

void EncoderRegistry::resetForTesting() {
    CheckState& state = checkState();
    ++state.generation;
    state.started = false;
    {
        QMutexLocker lock(&state.mutex);
        state.support.clear();
    }
    state.offered = offeredNow();
}
