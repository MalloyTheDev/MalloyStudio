#pragma once

#include <QElapsedTimer>
#include <QString>
#include <QStringList>

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>

// ---------------------------------------------------------------------------
// FrameProfile: where the time goes between a captured frame and ffmpeg.
//
// The six stage counters say how many frames survived each stage. They cannot
// say which stage was slow, and the obvious substitute is not available: two
// runs of an identical workload delivered 2.7 and 49.9 frames a second, so the
// delivered rate measures the machine's mood rather than the pipeline's shape.
// What distinguishes the stages is how long each one takes and how deep its
// queue gets, which is what this records.
//
// Distributions rather than averages, because the variance is the thing being
// investigated. A mean hides a stage that is usually free and occasionally
// costs forty milliseconds, and that is exactly the shape a stall has.
//
// Off unless switched on. Reading a clock twice per stage is cheap next to
// moving eight megabytes, but this is scaffolding for one investigation and
// should not be a tax on every recording afterwards.
// ---------------------------------------------------------------------------
namespace FrameProfile {

enum class Stage {
    // Time inside the backend's own callback: the GPU copy, the map, and the
    // memcpy into a QImage.
    CaptureReadback,
    // From the backend handing a frame over to the consumer thread picking it
    // up. This is not work, it is waiting, and it measures how congested the
    // GUI thread is rather than anything about capture.
    HandoffToGui,
    // Allocating the canvas, clearing it, and drawing the sources into it.
    Composition,
    // Drawing the finished canvas into the widget, scaled. Work done for the
    // preview on screen, charged to the same thread the recording depends on,
    // which is why it is timed apart from composition rather than with it.
    WidgetBlit,
    // Un-premultiplying the canvas into the format declared on ffmpeg's stdin.
    EncoderConvert,
    // Handing the frame to the video transport. Since the transport owns a
    // thread of its own this is a queue insertion, not a write.
    EncoderWrite,
    // The write the transport thread actually performed into the pipe. This is
    // where blocking on ffmpeg shows up, and it is deliberately not on the
    // thread that composes.
    PipeWrite,
    // Bytes waiting to be written to ffmpeg, sampled once per tick. Not a
    // duration: this is the queue depth that says whether the far end is
    // keeping up.
    EncoderBacklogBytes,
    // Wall clock between consecutive encoder ticks. The timer asks for a fixed
    // interval; the gap between what it asked for and what it got is how
    // starved the event loop is.
    TickGap,
    Count,
};

namespace detail {

// Buckets are powers of two subdivided into four, so neighbouring buckets are
// about 19% apart. Coarser than that cannot tell a 1.4 ms stage from a 2.1 ms
// one, which is the distinction the whole exercise turns on.
inline constexpr int kMantissaBits = 2;
inline constexpr int kBuckets = 160;

// Small values are their own bucket, and above that each octave is split into
// four. The two ranges have to meet without a gap: an encoding where some
// indices are unreachable leaves their floors out of order, and a percentile
// walk over unordered edges reports nonsense.
inline int bucketFor(qint64 value) {
    if (value <= 0) return 0;
    const auto v = static_cast<quint64>(value);
    if (v < (1u << kMantissaBits)) return static_cast<int>(v);

    const int exponent = 63 - std::countl_zero(v);
    const int shift    = exponent - kMantissaBits;
    const int mantissa = static_cast<int>((v >> shift) & ((1 << kMantissaBits) - 1));
    const int index    = ((shift + 1) << kMantissaBits) + mantissa;
    return index < kBuckets ? index : kBuckets - 1;
}

// The smallest value that lands in a bucket. Percentiles are reported at this
// edge, so a quoted figure is always one a real sample reached or passed.
inline qint64 bucketFloor(int index) {
    if (index < (1 << kMantissaBits)) return index;
    const int shift    = (index >> kMantissaBits) - 1;
    const int mantissa = index & ((1 << kMantissaBits) - 1);
    return (qint64((1 << kMantissaBits) + mantissa)) << shift;
}

struct Stat {
    std::atomic<quint64> count{0};
    std::atomic<quint64> total{0};
    std::atomic<qint64>  max{0};
    std::array<std::atomic<quint32>, kBuckets> buckets{};

    void add(qint64 value) {
        count.fetch_add(1, std::memory_order_relaxed);
        total.fetch_add(static_cast<quint64>(value < 0 ? 0 : value),
                        std::memory_order_relaxed);
        qint64 seen = max.load(std::memory_order_relaxed);
        while (value > seen
               && !max.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
        }
        buckets[static_cast<size_t>(bucketFor(value))]
            .fetch_add(1, std::memory_order_relaxed);
    }

    void clear() {
        count.store(0, std::memory_order_relaxed);
        total.store(0, std::memory_order_relaxed);
        max.store(0, std::memory_order_relaxed);
        for (auto& b : buckets) b.store(0, std::memory_order_relaxed);
    }

    // The smallest value at or below which the given fraction of samples fell.
    qint64 percentile(double fraction) const {
        const quint64 n = count.load(std::memory_order_relaxed);
        if (n == 0) return 0;
        const auto target = static_cast<quint64>(double(n) * fraction);
        quint64 seen = 0;
        for (int i = 0; i < kBuckets; ++i) {
            seen += buckets[static_cast<size_t>(i)].load(std::memory_order_relaxed);
            if (seen > target) return bucketFloor(i);
        }
        return max.load(std::memory_order_relaxed);
    }

    double mean() const {
        const quint64 n = count.load(std::memory_order_relaxed);
        if (n == 0) return 0.0;
        return double(total.load(std::memory_order_relaxed)) / double(n);
    }
};

inline std::atomic<bool>& enabledFlag() {
    static std::atomic<bool> flag{false};
    return flag;
}

// Two sets of the same stages: one for the whole run, one for the second being
// reported. The window is what makes a time series possible, and a time series
// is what shows a stage and a queue moving together.
inline std::array<Stat, size_t(Stage::Count)>& cumulative() {
    static std::array<Stat, size_t(Stage::Count)> stats;
    return stats;
}

inline std::array<Stat, size_t(Stage::Count)>& window() {
    static std::array<Stat, size_t(Stage::Count)> stats;
    return stats;
}

inline const char* name(Stage stage) {
    switch (stage) {
        case Stage::CaptureReadback:     return "CAPTURE READBACK";
        case Stage::HandoffToGui:        return "HANDOFF TO GUI";
        case Stage::Composition:         return "COMPOSITION";
        case Stage::WidgetBlit:          return "WIDGET BLIT";
        case Stage::EncoderConvert:      return "ENCODER CONVERT";
        case Stage::EncoderWrite:        return "ENCODER HANDOFF";
        case Stage::PipeWrite:           return "PIPE WRITE";
        case Stage::EncoderBacklogBytes: return "ENCODER BACKLOG";
        case Stage::TickGap:             return "TICK GAP";
        case Stage::Count:               break;
    }
    return "?";
}

inline bool isDuration(Stage stage) {
    return stage != Stage::EncoderBacklogBytes;
}

}  // namespace detail

inline bool enabled() {
    return detail::enabledFlag().load(std::memory_order_relaxed);
}

inline void setEnabled(bool on) {
    detail::enabledFlag().store(on, std::memory_order_relaxed);
}

inline void record(Stage stage, qint64 value) {
    if (!enabled()) return;
    detail::cumulative()[size_t(stage)].add(value);
    detail::window()[size_t(stage)].add(value);
}

inline void reset() {
    for (auto& s : detail::cumulative()) s.clear();
    for (auto& s : detail::window()) s.clear();
}

// Times a scope and files it under a stage. Costs two clock reads when
// profiling is on and one atomic load when it is off.
class Scoped {
public:
    explicit Scoped(Stage stage) : m_stage(stage) {
        if (enabled()) m_timer.start();
    }
    ~Scoped() {
        if (m_timer.isValid()) record(m_stage, m_timer.nsecsElapsed());
    }
    Scoped(const Scoped&) = delete;
    Scoped& operator=(const Scoped&) = delete;

private:
    Stage         m_stage;
    QElapsedTimer m_timer;
};

// One line per stage: how often, how long typically, and how long at the tail.
// The tail is the column to read, because a pipeline is limited by what it does
// on a bad frame rather than on a median one.
inline QString report() {
    QStringList lines;
    lines << QStringLiteral("frame profile (per stage, microseconds unless noted)")
          << QStringLiteral("%1 %2 %3 %4 %5 %6")
                 .arg(QStringLiteral("stage"), -18)
                 .arg(QStringLiteral("count"), 8)
                 .arg(QStringLiteral("mean"), 10)
                 .arg(QStringLiteral("p50"), 10)
                 .arg(QStringLiteral("p95"), 10)
                 .arg(QStringLiteral("max"), 10);
    for (int i = 0; i < int(Stage::Count); ++i) {
        const auto stage = Stage(i);
        const detail::Stat& s = detail::cumulative()[size_t(i)];
        if (s.count.load(std::memory_order_relaxed) == 0) continue;
        const double scale = detail::isDuration(stage) ? 1000.0 : 1024.0;
        lines << QStringLiteral("%1 %2 %3 %4 %5 %6")
                     .arg(QLatin1String(detail::name(stage)), -18)
                     .arg(s.count.load(std::memory_order_relaxed), 8)
                     .arg(s.mean() / scale, 10, 'f', 1)
                     .arg(double(s.percentile(0.50)) / scale, 10, 'f', 1)
                     .arg(double(s.percentile(0.95)) / scale, 10, 'f', 1)
                     .arg(double(s.max.load(std::memory_order_relaxed)) / scale, 10, 'f', 1);
    }
    lines << QStringLiteral("backlog figures are kibibytes, not microseconds");
    return lines.join(QLatin1Char('\n'));
}

// The last second, as one line, and then the window starts again. Emitted
// while a run is in progress so a stage can be seen moving with a queue rather
// than only averaged against it.
inline QString seriesLine() {
    QStringList parts;
    for (int i = 0; i < int(Stage::Count); ++i) {
        const auto stage = Stage(i);
        detail::Stat& s = detail::window()[size_t(i)];
        const quint64 n = s.count.load(std::memory_order_relaxed);
        if (n == 0) continue;
        const double scale = detail::isDuration(stage) ? 1000.0 : 1024.0;
        parts << QStringLiteral("%1 n=%2 mean=%3 max=%4")
                     .arg(QLatin1String(detail::name(stage)))
                     .arg(n)
                     .arg(s.mean() / scale, 0, 'f', 1)
                     .arg(double(s.max.load(std::memory_order_relaxed)) / scale, 0, 'f', 1);
        s.clear();
    }
    return parts.join(QStringLiteral("  |  "));
}

}  // namespace FrameProfile
