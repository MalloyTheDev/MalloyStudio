#include "ui/workspaces/EditorWorkspace.h"
#include "ui/components/PanelFrame.h"
#include "ui/components/Placeholder.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"
#include "project/MediaRegistry.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kRulerH      = 26;
constexpr int kTrackH      = 44;
constexpr int kHeaderW     = 200;
constexpr int kTimelineLen = 360;     // seconds
constexpr int kEdgePx      = 6;       // clip trim grip width
constexpr double kFps      = 60.0;    // playback ticker FPS

// Private MIME used by the Media Bin → TimelineCanvas drag.
const char* const kMediaPathMime = "application/x-malloy-mediapath";

struct Track { QString id; QString name; bool audio; };
const QVector<Track>& tracks() {
    static const QVector<Track> t = {
        {QStringLiteral("V3"), QStringLiteral("Titles"),    false},
        {QStringLiteral("V2"), QStringLiteral("Webcam"),    false},
        {QStringLiteral("V1"), QStringLiteral("Gameplay"),  false},
        {QStringLiteral("A1"), QStringLiteral("Mic"),       true},
        {QStringLiteral("A2"), QStringLiteral("Game Audio"),true},
        {QStringLiteral("A3"), QStringLiteral("Music Bed"), true},
    };
    return t;
}

struct Clip {
    int     track    = 0;
    double  start    = 0.0;
    double  dur      = 0.0;
    QString label;
    QString tag;
    QColor  color;
    bool    audio    = false;
    // ── Inspector-bound per-clip params (v4). All defaulted so legacy clips
    // (loaded from .malloy.json files written before v4) round-trip unchanged.
    int     tx       = 0;            // transform.x  (px in canvas space)
    int     ty       = 0;            // transform.y
    double  scale    = 100.0;        // transform.scale  (percent, 1..1000)
    double  rotation = 0.0;          // transform.rotation (degrees, -360..360)
    int     opacity  = 100;          // transform.opacity (percent, 0..100)
    int     gainDb   = 0;            // audioParams.gainDb (-30..12)
    int     pan      = 0;            // audioParams.pan (-100..100)
    int     channels = 0;            // audioParams.channels (0=Stereo,1=MonoL,2=MonoR)
    double  speedFactor = 1.0;       // speed.factor (0.10..4.00)
};
const QVector<Clip>& clips() {
    const QColor violet(150, 110, 210), blue(90, 150, 205), blueD(80, 135, 195);
    const QColor green(95, 190, 130), greenD(90, 170, 120), amber(200, 165, 90);
    static const QVector<Clip> c = {
        {0, 0, 8, QStringLiteral("Intro card"), QStringLiteral("IMG"), violet, false},
        {0, 60, 4, QStringLiteral("Lower-third · No-hit"), QStringLiteral("IMG"), violet, false},
        {0, 180, 4, QStringLiteral("Lower-third · Dodge"), QStringLiteral("IMG"), violet, false},
        {1, 8, 120, QStringLiteral("Webcam α7C"), QStringLiteral("VID"), blue, false},
        {1, 140, 96, QStringLiteral("Webcam · phase 3"), QStringLiteral("VID"), blue, false},
        {2, 0, 8, QStringLiteral("Intro · BG"), QStringLiteral("VID"), blueD, false},
        {2, 8, 132, QStringLiteral("Spire — phase 1"), QStringLiteral("VID"), blueD, false},
        {2, 140, 60, QStringLiteral("Spire — phase 2"), QStringLiteral("VID"), blueD, false},
        {2, 200, 88, QStringLiteral("Spire — phase 3"), QStringLiteral("VID"), blueD, false},
        {3, 8, 280, QStringLiteral("Mic — SM7B"), QStringLiteral("AUD"), green, true},
        {4, 8, 280, QStringLiteral("Game audio"), QStringLiteral("AUD"), greenD, true},
        {5, 0, 64, QStringLiteral("Music · Synth bed"), QStringLiteral("AUD"), amber, true},
        {5, 200, 90, QStringLiteral("Music · Outro"), QStringLiteral("AUD"), amber, true},
    };
    return c;
}

QJsonObject clipToJson(const Clip& c) {
    QJsonObject o;
    o.insert(QStringLiteral("track"), c.track);
    o.insert(QStringLiteral("start"), c.start);
    o.insert(QStringLiteral("dur"),   c.dur);
    o.insert(QStringLiteral("label"), c.label);
    o.insert(QStringLiteral("tag"),   c.tag);
    o.insert(QStringLiteral("color"), c.color.name());
    o.insert(QStringLiteral("audio"), c.audio);
    // v4 nested per-clip params.
    QJsonObject xf;
    xf.insert(QStringLiteral("x"),        c.tx);
    xf.insert(QStringLiteral("y"),        c.ty);
    xf.insert(QStringLiteral("scale"),    c.scale);
    xf.insert(QStringLiteral("rotation"), c.rotation);
    xf.insert(QStringLiteral("opacity"),  c.opacity);
    o.insert(QStringLiteral("transform"), xf);
    QJsonObject ap;
    ap.insert(QStringLiteral("gainDb"),   c.gainDb);
    ap.insert(QStringLiteral("pan"),      c.pan);
    ap.insert(QStringLiteral("channels"), c.channels);
    o.insert(QStringLiteral("audioParams"), ap);
    QJsonObject sp;
    sp.insert(QStringLiteral("factor"),   c.speedFactor);
    o.insert(QStringLiteral("speed"), sp);
    return o;
}
Clip clipFromJson(const QJsonObject& o) {
    Clip c;
    c.track = o.value(QStringLiteral("track")).toInt();
    c.start = o.value(QStringLiteral("start")).toDouble();
    c.dur   = o.value(QStringLiteral("dur")).toDouble();
    c.label = o.value(QStringLiteral("label")).toString();
    c.tag   = o.value(QStringLiteral("tag")).toString();
    c.color = QColor(o.value(QStringLiteral("color")).toString());
    c.audio = o.value(QStringLiteral("audio")).toBool();
    // v4 nested params: missing keys take the field defaults (back-compat).
    const QJsonObject xf = o.value(QStringLiteral("transform")).toObject();
    if (!xf.isEmpty()) {
        c.tx       = xf.value(QStringLiteral("x")).toInt(c.tx);
        c.ty       = xf.value(QStringLiteral("y")).toInt(c.ty);
        c.scale    = xf.value(QStringLiteral("scale")).toDouble(c.scale);
        c.rotation = xf.value(QStringLiteral("rotation")).toDouble(c.rotation);
        c.opacity  = xf.value(QStringLiteral("opacity")).toInt(c.opacity);
    }
    const QJsonObject ap = o.value(QStringLiteral("audioParams")).toObject();
    if (!ap.isEmpty()) {
        c.gainDb   = ap.value(QStringLiteral("gainDb")).toInt(c.gainDb);
        c.pan      = ap.value(QStringLiteral("pan")).toInt(c.pan);
        c.channels = ap.value(QStringLiteral("channels")).toInt(c.channels);
    }
    const QJsonObject sp = o.value(QStringLiteral("speed")).toObject();
    if (!sp.isEmpty()) {
        c.speedFactor = sp.value(QStringLiteral("factor")).toDouble(c.speedFactor);
    }
    return c;
}
struct Marker { double t; QColor color; };
const QVector<Marker>& markers() {
    static const QVector<Marker> m = {
        {60, Theme::Accent}, {140, Theme::Warn}, {200, Theme::Rec},
    };
    return m;
}

QLabel* lbl(const QString& s, const QString& tone = QString(), int px = 13, bool bold = false, bool mono = false) {
    return Theme::label(s, tone, px, bold, mono);
}

QFrame* iconChip(const QString& name, const QColor& color, int chip, int ic) {
    auto* f = new QFrame; f->setObjectName(QStringLiteral("iconChip")); f->setFixedSize(chip, chip);
    auto* v = new QVBoxLayout(f); v->setContentsMargins(0, 0, 0, 0);
    auto* l = new QLabel; l->setAlignment(Qt::AlignCenter); l->setPixmap(Icons::pixmap(name, color, ic));
    v->addWidget(l);
    return f;
}

// Format a seconds value as "HH:MM:SS:FF" with FF computed at kFps. The total
// label keeps the same shape ("HH:MM:SS:FF") so the two labels align.
QString formatTimecode(double seconds) {
    if (seconds < 0) seconds = 0;
    const int totalFrames = int(std::round(seconds * kFps));
    const int ff = totalFrames % int(kFps);
    const int totalSec = totalFrames / int(kFps);
    const int ss = totalSec % 60;
    const int mm = (totalSec / 60) % 60;
    const int hh = totalSec / 3600;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(hh, 2, 10, QChar('0'))
        .arg(mm, 2, 10, QChar('0'))
        .arg(ss, 2, 10, QChar('0'))
        .arg(ff, 2, 10, QChar('0'));
}

}  // namespace

// ── Custom-painted timeline canvas ──────────────────────────────────────────
//
// Promoted to a file-scope Q_OBJECT class so it can emit selectionChanged /
// playheadChanged / playingChanged / clipsChanged into EditorWorkspace. The
// class is only referenced from this TU (header forward-declares it as QWidget*)
// — moc requires a non-anonymous declaration to generate the meta-object, so it
// can't live in the anonymous namespace above.
class TimelineCanvas : public QWidget {
    Q_OBJECT
public:
    enum class Tool { Select, Razor, Slip, Hand };

    explicit TimelineCanvas(QWidget* parent = nullptr);

    void setZoom(double z) { m_zoom = z; updateSize(); update(); }
    void setTool(Tool t);
    void setSnap(bool on) { m_snap = on; }

    // Project timeline serialization (persisted inside the .malloy.json).
    QJsonArray toJson() const;
    void setClips(const QJsonArray& arr);

    // ── Selection-bound accessors (read by Inspector on selectionChanged) ──
    int selectedIndex() const { return m_selected; }
    bool selectedIsAudio() const {
        return m_selected >= 0 && m_selected < m_clips.size() && m_clips[m_selected].audio;
    }
    QString selectedLabel() const {
        return (m_selected >= 0 && m_selected < m_clips.size()) ? m_clips[m_selected].label : QString();
    }
    int    selectedField_tx()       const { return clipField([](const Clip& c){ return c.tx; }, 0); }
    int    selectedField_ty()       const { return clipField([](const Clip& c){ return c.ty; }, 0); }
    double selectedField_scale()    const { return clipField([](const Clip& c){ return c.scale; }, 100.0); }
    double selectedField_rotation() const { return clipField([](const Clip& c){ return c.rotation; }, 0.0); }
    int    selectedField_opacity()  const { return clipField([](const Clip& c){ return c.opacity; }, 100); }
    int    selectedField_gainDb()   const { return clipField([](const Clip& c){ return c.gainDb; }, 0); }
    int    selectedField_pan()      const { return clipField([](const Clip& c){ return c.pan; }, 0); }
    int    selectedField_channels() const { return clipField([](const Clip& c){ return c.channels; }, 0); }
    double selectedField_speed()    const { return clipField([](const Clip& c){ return c.speedFactor; }, 1.0); }

    // ── Inspector write-back. Each mutator updates a single per-clip field,
    // repaints, and emits clipsChanged so the project's dirty flag fires.
    void setField_tx(int v)         { applyField([v](Clip& c){ c.tx = v; }); }
    void setField_ty(int v)         { applyField([v](Clip& c){ c.ty = v; }); }
    void setField_scale(double v)   { applyField([v](Clip& c){ c.scale = v; }); }
    void setField_rotation(double v){ applyField([v](Clip& c){ c.rotation = v; }); }
    void setField_opacity(int v)    { applyField([v](Clip& c){ c.opacity = v; }); }
    void setField_gainDb(int v)     { applyField([v](Clip& c){ c.gainDb = v; }); }
    void setField_pan(int v)        { applyField([v](Clip& c){ c.pan = v; }); }
    void setField_channels(int v)   { applyField([v](Clip& c){ c.channels = v; }); }
    void setField_speed(double v)   { applyField([v](Clip& c){ c.speedFactor = v; }); }

    // ── Transport ──
    double playhead() const { return m_playhead; }
    bool   isPlaying() const { return m_playing; }
    void   togglePlay()  { m_playing ? pause() : play(); }
    void   play();
    void   pause();
    void   skipBack();
    void   skipForward();
    void   frameStep(int frames);   // frames at kFps; negative steps back
    void   splitAtPlayhead();

signals:
    void selectionChanged(int idx);
    void playheadChanged(double t);
    void playingChanged(bool playing);
    void clipsChanged();

protected:
    QSize sizeHint() const override { return minimumSize(); }
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private:
    enum class Zone { None, Body, LeftEdge, RightEdge };

    void   updateSize();
    int    clipAt(const QPointF& pos, Zone* zone) const;
    void   updateHoverCursor(const QPointF& pos);
    double snapTime(double t, int ignoreClip) const;
    void   splitClip(int i, double t);
    double timelineEnd() const;
    void   tickPlayhead();
    void   setSelected(int idx);

    // Helper: read selected clip's field with a default when no selection.
    template <typename Fn, typename T>
    T clipField(Fn fn, T fallback) const {
        if (m_selected < 0 || m_selected >= m_clips.size()) return fallback;
        return fn(m_clips[m_selected]);
    }
    // Helper: mutate the selected clip's field with a callback.
    template <typename Fn>
    void applyField(Fn fn) {
        if (m_selected < 0 || m_selected >= m_clips.size()) return;
        fn(m_clips[m_selected]);
        update();
        emit clipsChanged();
    }

    double m_zoom      = 60.0;     // px/sec
    double m_playhead  = 0.0;      // sec
    int    m_selected  = -1;
    bool   m_scrubbing = false;

    QVector<Clip> m_clips;
    Tool m_tool      = Tool::Select;
    bool m_snap      = true;
    int  m_drag      = -1;
    Zone m_dragZone  = Zone::None;
    double m_origStart = 0, m_origDur = 0, m_grabOffset = 0;

    // Drift-compensated play ticker: a 16 ms QTimer drives tickPlayhead(),
    // which advances by REAL elapsed seconds via QElapsedTimer. A stalled UI
    // thread cannot slow logical playback below ~kFps.
    QTimer*       m_playTimer = nullptr;
    QElapsedTimer m_playClock;
    bool          m_playing   = false;

    // Drop-preview: track currently hovered during an in-flight bin drag.
    int m_dropTrack = -1;
};

// ── TimelineCanvas implementation ──

TimelineCanvas::TimelineCanvas(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setAcceptDrops(true);
    m_clips = clips();        // seed; the active project replaces it via setClips()
    updateSize();
    m_playTimer = new QTimer(this);
    m_playTimer->setInterval(int(1000.0 / kFps));
    connect(m_playTimer, &QTimer::timeout, this, &TimelineCanvas::tickPlayhead);
}

void TimelineCanvas::setTool(Tool t) {
    m_tool = t;
    setCursor(t == Tool::Razor ? Qt::SplitHCursor : Qt::ArrowCursor);
}

QJsonArray TimelineCanvas::toJson() const {
    QJsonArray a;
    for (const Clip& c : m_clips) a.append(clipToJson(c));
    return a;
}

void TimelineCanvas::setClips(const QJsonArray& arr) {
    // Stop play before we mutate the vector — tick() reads start+dur during
    // timelineEnd() and a clear()-then-push race would crash if play ticked
    // between (single-threaded so the data race itself is impossible, but
    // pausing keeps the play button + timecode in a coherent state).
    if (m_playing) pause();
    m_clips.clear();
    for (const auto& v : arr) m_clips.push_back(clipFromJson(v.toObject()));
    m_selected = -1;
    m_drag = -1;
    update();
    emit selectionChanged(-1);
}

void TimelineCanvas::play() {
    if (m_playing) return;
    if (m_playhead >= timelineEnd()) m_playhead = 0;
    m_playing = true;
    m_playClock.start();
    m_playTimer->start();
    emit playingChanged(true);
}

void TimelineCanvas::pause() {
    if (!m_playing) return;
    m_playing = false;
    m_playTimer->stop();
    emit playingChanged(false);
}

void TimelineCanvas::skipBack() {
    m_playhead = 0;
    update();
    emit playheadChanged(m_playhead);
}

void TimelineCanvas::skipForward() {
    m_playhead = timelineEnd();
    update();
    emit playheadChanged(m_playhead);
}

void TimelineCanvas::frameStep(int frames) {
    const double end = timelineEnd();
    m_playhead = qBound(0.0, m_playhead + double(frames) / kFps, end);
    update();
    emit playheadChanged(m_playhead);
}

void TimelineCanvas::splitAtPlayhead() {
    // Prefer the selected clip if the playhead is inside it; else split whichever
    // clip the playhead is currently over (search all tracks).
    int target = -1;
    if (m_selected >= 0 && m_selected < m_clips.size()) {
        const Clip& c = m_clips[m_selected];
        if (m_playhead > c.start && m_playhead < c.start + c.dur) target = m_selected;
    }
    if (target < 0) {
        for (int i = 0; i < m_clips.size(); ++i) {
            const Clip& c = m_clips[i];
            if (m_playhead > c.start && m_playhead < c.start + c.dur) { target = i; break; }
        }
    }
    if (target < 0) return;
    splitClip(target, m_playhead);
    update();
    emit clipsChanged();
    emit selectionChanged(m_selected);
}

double TimelineCanvas::timelineEnd() const {
    double end = 0;
    for (const Clip& c : m_clips) end = std::max(end, c.start + c.dur);
    if (end <= 0) end = double(kTimelineLen);
    return end;
}

void TimelineCanvas::tickPlayhead() {
    // Advance by actual elapsed time (drift-safe). nsecsElapsed is restarted on
    // each tick so any UI stall is folded into the next advance, not lost.
    const qint64 ns = m_playClock.nsecsElapsed();
    m_playClock.restart();
    m_playhead += double(ns) / 1.0e9;
    const double end = timelineEnd();
    if (m_playhead >= end) {
        m_playhead = end;
        pause();
    }
    update();
    emit playheadChanged(m_playhead);
}

void TimelineCanvas::setSelected(int idx) {
    if (idx == m_selected) return;
    m_selected = idx;
    update();
    emit selectionChanged(m_selected);
}

void TimelineCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), Theme::Surface0);

    const int contentW = int(kTimelineLen * m_zoom);

    // Tracks background + gridlines
    for (int ti = 0; ti < tracks().size(); ++ti) {
        const int y = kRulerH + ti * kTrackH;
        p.fillRect(QRect(0, y, contentW, kTrackH),
                   tracks()[ti].audio ? QColor(0x20, 0x21, 0x24) : Theme::Surface0);
        for (int s = 0; s <= kTimelineLen; s += 10) {
            const int x = int(s * m_zoom);
            p.setPen(QPen(s % 60 == 0 ? QColor(0x2d, 0x2f, 0x33) : QColor(0x22, 0x23, 0x26),
                          1, s % 60 == 0 ? Qt::SolidLine : Qt::DotLine));
            p.drawLine(x, y, x, y + kTrackH);
        }
        p.setPen(Theme::Border);
        p.drawLine(0, y + kTrackH, contentW, y + kTrackH);
    }

    // Highlight the drop-target track during an in-flight media drag.
    if (m_dropTrack >= 0 && m_dropTrack < tracks().size()) {
        const int y = kRulerH + m_dropTrack * kTrackH;
        p.fillRect(QRect(0, y, contentW, kTrackH), QColor(87, 155, 255, 28));
    }

    // Clips
    const auto& cs = m_clips;
    for (int i = 0; i < cs.size(); ++i) {
        const Clip& c = cs[i];
        const QRectF r(c.start * m_zoom + 1, kRulerH + c.track * kTrackH + 3,
                       c.dur * m_zoom - 2, kTrackH - 6);
        QPainterPath path; path.addRoundedRect(r, 3, 3);
        p.fillPath(path, c.color);
        if (i == m_selected) {
            p.setPen(QPen(QColor(0xf7, 0xf7, 0xf7), 1.5));
            p.drawPath(path);
        } else {
            p.setPen(QPen(QColor(0, 0, 0, 80), 1));
            p.drawPath(path);
        }
        // label + tag (dark text on colored clip)
        p.setPen(QColor(0x12, 0x12, 0x12));
        QFont lf = p.font(); lf.setPixelSize(9); lf.setWeight(QFont::DemiBold); p.setFont(lf);
        const QRectF tr = r.adjusted(6, 2, -6, -2);
        if (tr.width() > 24) {
            p.drawText(tr, Qt::AlignLeft | Qt::AlignTop,
                       p.fontMetrics().elidedText(c.label, Qt::ElideRight, int(tr.width()) - 24));
            p.drawText(tr, Qt::AlignRight | Qt::AlignTop, c.tag);
        }
        // audio waveform mock
        if (c.audio && r.width() > 12) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 130));
            const int bars = int(r.width() / 3);
            const double cy = r.center().y() + 4;
            for (int b = 0; b < bars; ++b) {
                const double h = 2 + std::abs(std::sin(b * 0.7) * 0.7 + std::sin(b * 0.13) * 0.4) * 10;
                p.drawRect(QRectF(r.left() + 4 + b * 3, cy - h / 2, 2, h));
            }
        }
    }

    // Ruler band
    p.fillRect(QRect(0, 0, contentW, kRulerH), Theme::Surface1);
    p.setPen(Theme::Border);
    p.drawLine(0, kRulerH, contentW, kRulerH);
    const int major = m_zoom > 100 ? 1 : m_zoom > 50 ? 2 : m_zoom > 25 ? 5 : 10;
    QFont rf = p.font(); rf.setPixelSize(10); rf.setWeight(QFont::Normal); p.setFont(rf);
    for (int s = 0; s <= kTimelineLen; s += major) {
        const int x = int(s * m_zoom);
        p.setPen(Theme::Border);
        p.drawLine(x, 0, x, kRulerH);
        p.setPen(Theme::TextMute);
        p.drawText(x + 4, 14, QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0')));
    }
    // Markers
    for (const Marker& m : markers()) {
        const int x = int(m.t * m_zoom);
        QPainterPath tri;
        tri.moveTo(x - 5, kRulerH - 8); tri.lineTo(x + 5, kRulerH - 8); tri.lineTo(x, kRulerH);
        tri.closeSubpath();
        p.fillPath(tri, m.color);
    }

    // Playhead
    const int px = int(m_playhead * m_zoom);
    p.setPen(QPen(Theme::RecHi, 1));
    p.drawLine(px, 0, px, height());
    QPainterPath head;
    head.moveTo(px - 6, 0); head.lineTo(px + 6, 0);
    head.lineTo(px + 6, kRulerH * 0.6); head.lineTo(px, kRulerH);
    head.lineTo(px - 6, kRulerH * 0.6); head.closeSubpath();
    p.fillPath(head, Theme::Rec);
}

void TimelineCanvas::mousePressEvent(QMouseEvent* e) {
    const QPointF pos = e->position();
    if (pos.y() < kRulerH) {                        // ruler → scrub playhead
        if (m_playing) pause();                     // scrubbing implicitly stops play
        m_scrubbing = true;
        m_playhead = qBound(0.0, pos.x() / m_zoom, double(kTimelineLen));
        update();
        emit playheadChanged(m_playhead);
        return;
    }
    Zone zone = Zone::None;
    const int i = clipAt(pos, &zone);
    if (m_tool == Tool::Razor) {                    // razor → split at click
        if (i >= 0) {
            splitClip(i, pos.x() / m_zoom);
            update();
            emit clipsChanged();
            emit selectionChanged(m_selected);
        } else {
            update();
        }
        return;
    }
    setSelected(i);
    if (i >= 0) {                                   // select → begin move/trim
        m_drag = i;
        m_dragZone = zone;
        m_origStart = m_clips[i].start;
        m_origDur = m_clips[i].dur;
        m_grabOffset = pos.x() / m_zoom - m_clips[i].start;
    }
    update();
}

void TimelineCanvas::mouseMoveEvent(QMouseEvent* e) {
    const QPointF pos = e->position();
    if (m_scrubbing) {
        m_playhead = qBound(0.0, pos.x() / m_zoom, double(kTimelineLen));
        update();
        emit playheadChanged(m_playhead);
        return;
    }
    if (m_drag >= 0 && (e->buttons() & Qt::LeftButton)) {
        Clip& c = m_clips[m_drag];
        const double t = pos.x() / m_zoom;
        const double origEnd = m_origStart + m_origDur;
        constexpr double minDur = 0.25;
        if (m_dragZone == Zone::Body) {
            c.start = qBound(0.0, snapTime(t - m_grabOffset, m_drag),
                             double(kTimelineLen) - c.dur);
            const int ty = qBound(0, (int(pos.y()) - kRulerH) / kTrackH,
                                  int(tracks().size()) - 1);
            if (tracks()[ty].audio == c.audio) c.track = ty;
        } else if (m_dragZone == Zone::LeftEdge) {
            const double ns = qBound(0.0, snapTime(t, m_drag), origEnd - minDur);
            c.start = ns;
            c.dur = origEnd - ns;
        } else if (m_dragZone == Zone::RightEdge) {
            const double ne = qBound(c.start + minDur, snapTime(t, m_drag),
                                     double(kTimelineLen));
            c.dur = ne - c.start;
        }
        update();
        emit clipsChanged();
        return;
    }
    if (e->buttons() == Qt::NoButton) updateHoverCursor(pos);
}

void TimelineCanvas::mouseReleaseEvent(QMouseEvent*) {
    m_scrubbing = false;
    m_drag = -1;
}

void TimelineCanvas::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasFormat(QLatin1String(kMediaPathMime))) {
        e->acceptProposedAction();
        m_dropTrack = qBound(0, (int(e->position().y()) - kRulerH) / kTrackH,
                             int(tracks().size()) - 1);
        update();
    }
}

void TimelineCanvas::dragMoveEvent(QDragMoveEvent* e) {
    if (!e->mimeData()->hasFormat(QLatin1String(kMediaPathMime))) return;
    e->acceptProposedAction();
    const int t = qBound(0, (int(e->position().y()) - kRulerH) / kTrackH,
                         int(tracks().size()) - 1);
    if (t != m_dropTrack) { m_dropTrack = t; update(); }
}

void TimelineCanvas::dropEvent(QDropEvent* e) {
    const QByteArray payload = e->mimeData()->data(QLatin1String(kMediaPathMime));
    m_dropTrack = -1;
    if (payload.isEmpty()) { update(); return; }

    // Payload format: "path\nkind\ndurSecs\nname" (newline-separated). kind:
    // 0=Video,1=Audio,2=Image,3=Other (matches MediaInfo::Kind). durSecs is a
    // textual integer; 0 ⇒ unknown, falls back to 4 s.
    const QList<QByteArray> parts = payload.split('\n');
    const QString path     = parts.value(0);
    const int     kind     = parts.value(1).toInt();
    const int     durSecs  = parts.value(2).toInt();
    QString       name     = parts.value(3);
    if (name.isEmpty()) name = QFileInfo(path).fileName();
    const bool isAudio = (kind == 1);

    // Drop position → (track, start). Body of a track only — the ruler is reserved
    // for scrubbing. Track is clamped to a compatible audio/video lane.
    const int yTrack = qBound(0, (int(e->position().y()) - kRulerH) / kTrackH,
                              int(tracks().size()) - 1);
    int track = yTrack;
    if (tracks()[track].audio != isAudio) {
        // Find the first compatible track instead of refusing the drop.
        for (int i = 0; i < tracks().size(); ++i) {
            if (tracks()[i].audio == isAudio) { track = i; break; }
        }
    }
    const double t = qBound(0.0, e->position().x() / m_zoom, double(kTimelineLen));
    const double dur = (durSecs > 0) ? double(durSecs) : 4.0;

    Clip nc;
    nc.track = track;
    nc.start = qBound(0.0, snapTime(t, -1), double(kTimelineLen) - dur);
    nc.dur   = std::min(dur, double(kTimelineLen) - nc.start);
    nc.label = name;
    nc.audio = isAudio;
    switch (kind) {
        case 0: nc.tag = QStringLiteral("VID"); nc.color = QColor(90, 150, 205);  break;
        case 1: nc.tag = QStringLiteral("AUD"); nc.color = QColor(95, 190, 130);  break;
        case 2: nc.tag = QStringLiteral("IMG"); nc.color = QColor(150, 110, 210); break;
        default:nc.tag = QStringLiteral("FILE");nc.color = QColor(120, 130, 145); break;
    }
    m_clips.push_back(nc);
    setSelected(m_clips.size() - 1);
    e->acceptProposedAction();
    update();
    emit clipsChanged();
}

void TimelineCanvas::updateSize() {
    setMinimumSize(int(kTimelineLen * m_zoom), kRulerH + tracks().size() * kTrackH);
}

int TimelineCanvas::clipAt(const QPointF& pos, Zone* zone) const {
    if (zone) *zone = Zone::None;
    if (pos.y() < kRulerH) return -1;
    const int ti = (int(pos.y()) - kRulerH) / kTrackH;
    const double t = pos.x() / m_zoom;
    for (int i = 0; i < m_clips.size(); ++i) {
        const Clip& c = m_clips[i];
        if (c.track != ti) continue;
        if (t >= c.start && t <= c.start + c.dur) {
            if (zone) {
                const double edge = kEdgePx / m_zoom;
                if (t - c.start <= edge)              *zone = Zone::LeftEdge;
                else if (c.start + c.dur - t <= edge) *zone = Zone::RightEdge;
                else                                  *zone = Zone::Body;
            }
            return i;
        }
    }
    return -1;
}

void TimelineCanvas::updateHoverCursor(const QPointF& pos) {
    if (pos.y() < kRulerH) { setCursor(Qt::SizeHorCursor); return; }
    if (m_tool == Tool::Razor) { setCursor(Qt::SplitHCursor); return; }
    Zone z = Zone::None;
    const int i = clipAt(pos, &z);
    if (i < 0) { setCursor(Qt::ArrowCursor); return; }
    setCursor(z == Zone::LeftEdge || z == Zone::RightEdge ? Qt::SizeHorCursor
                                                          : Qt::OpenHandCursor);
}

double TimelineCanvas::snapTime(double t, int ignoreClip) const {
    if (!m_snap) return t;
    const double threshold = 8.0 / m_zoom;
    double best = t, bestDist = threshold;
    auto consider = [&](double cand) {
        const double d = std::abs(cand - t);
        if (d < bestDist) { bestDist = d; best = cand; }
    };
    consider(qRound(t));
    consider(m_playhead);
    for (int i = 0; i < m_clips.size(); ++i) {
        if (i == ignoreClip) continue;
        consider(m_clips[i].start);
        consider(m_clips[i].start + m_clips[i].dur);
    }
    return best;
}

void TimelineCanvas::splitClip(int i, double t) {
    if (i < 0 || i >= m_clips.size()) return;
    Clip a = m_clips[i];
    const double cut = snapTime(t, i);
    if (cut <= a.start + 0.1 || cut >= a.start + a.dur - 0.1) return;
    const double origEnd = a.start + a.dur;
    Clip b = a;
    a.dur = cut - a.start;
    b.start = cut;
    b.dur = origEnd - cut;
    m_clips[i] = a;
    m_clips.insert(i + 1, b);
    m_selected = i + 1;
}

// ── A QWidget bin-row that starts a QDrag with the media's path on press-drag.
//
// We can't put this in the anonymous namespace because it's a QObject with its
// own event handling — but it's only referenced from EditorWorkspace's ctor in
// this TU, so a file-scope class is fine.
class MediaBinRow : public QWidget {
public:
    MediaBinRow(const MediaInfo& m, QWidget* parent = nullptr)
        : QWidget(parent), m_path(m.filePath), m_kind(int(m.kind)),
          m_dur(m.durationSecs), m_name(m.name) {}

protected:
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton) m_pressPos = e->pos();
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!(e->buttons() & Qt::LeftButton)) return;
        if ((e->pos() - m_pressPos).manhattanLength() < 8) return;
        auto* mime = new QMimeData;
        QByteArray payload = m_path.toUtf8() + '\n'
                           + QByteArray::number(m_kind) + '\n'
                           + QByteArray::number(m_dur) + '\n'
                           + m_name.toUtf8();
        mime->setData(QLatin1String(kMediaPathMime), payload);
        auto* drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->exec(Qt::CopyAction);
    }

private:
    QString m_path;
    int     m_kind = 0;
    int     m_dur  = 0;
    QString m_name;
    QPoint  m_pressPos;
};

// ── EditorWorkspace ────────────────────────────────────────────────────────

EditorWorkspace::EditorWorkspace(MediaRegistry* media, QWidget* parent)
    : QWidget(parent), m_media(media)
{
    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    // ── Top half: media bin | preview | inspector ──────────────────────────
    auto* topSplit = new QSplitter(Qt::Horizontal, this);

    // ── Media bin (live, MediaRegistry-driven) ──
    auto* bin = new PanelFrame(tr("Media Bin"), QStringLiteral("media"));
    auto* countTag = Theme::makeTag(QStringLiteral("0"), QString());
    bin->addHeaderWidget(countTag);
    auto* binBody = new QWidget;
    auto* bv = new QVBoxLayout(binBody);
    bv->setContentsMargins(6, 6, 6, 6); bv->setSpacing(6);
    auto* search = new QLineEdit;
    search->setPlaceholderText(tr("Search media…"));
    search->setClearButtonEnabled(true);
    bv->addWidget(search);
    auto* rowsHost = new QWidget;
    auto* rowsLayout = new QVBoxLayout(rowsHost);
    rowsLayout->setContentsMargins(0, 0, 0, 0); rowsLayout->setSpacing(2);
    rowsLayout->addStretch();   // ONLY a trailing stretch in rowsLayout — the
                                // wipe loop in rebuildBin preserves this single
                                // item, and rows are inserted ahead of it.
    bv->addWidget(rowsHost, 1);
    // emptyHint deliberately lives in bv (the parent), NOT in rowsLayout.
    // Keeping it outside the wipe target means rebuildBin physically cannot
    // schedule it for deleteLater (which would dangle the captured pointer).
    auto* emptyHint = lbl(
        tr("No media found. Add a folder in the Media workspace."),
        QStringLiteral("mute"), 12);
    emptyHint->setWordWrap(true);
    emptyHint->setAlignment(Qt::AlignCenter);
    bv->addWidget(emptyHint);
    bin->bodyLayout()->addWidget(binBody);
    topSplit->addWidget(bin);

    // Rebuild bin rows from the registry, filtered by the search input.
    auto rebuildBin = [this, rowsLayout, emptyHint, countTag, search]() {
        // Wipe rows but keep the trailing stretch (last item). Without this
        // count()>1 guard, the next rebuild would have no stretch and the rows
        // would stack to the top of an unbounded layout.
        while (rowsLayout->count() > 1) {
            QLayoutItem* it = rowsLayout->takeAt(0);
            if (QWidget* w = it->widget()) w->deleteLater();
            delete it;
        }
        const QString needle = search->text().trimmed();
        int shown = 0;
        if (m_media) {
            for (const MediaInfo& mi : m_media->media()) {
                if (!needle.isEmpty()
                    && !mi.name.contains(needle, Qt::CaseInsensitive)) continue;
                auto* w = new MediaBinRow(mi);
                w->setObjectName(QStringLiteral("row"));
                w->setFixedHeight(36);
                w->setCursor(Qt::OpenHandCursor);
                w->setToolTip(mi.filePath);
                auto* h = new QHBoxLayout(w);
                h->setContentsMargins(8, 0, 8, 0); h->setSpacing(8);
                h->addWidget(iconChip(mi.iconName(), Theme::TextMute, 24, 11));
                auto* tv = new QVBoxLayout; tv->setSpacing(0);
                tv->addWidget(lbl(mi.name, QString(), 12));
                tv->addWidget(lbl(mi.propsText(), QStringLiteral("mute"), 10, false, true));
                h->addLayout(tv); h->addStretch();
                if (!QFile::exists(mi.filePath))
                    h->addWidget(Theme::makeTag(QStringLiteral("missing"), QStringLiteral("warn")));
                // Insert ahead of the trailing stretch (which is the last item).
                rowsLayout->insertWidget(rowsLayout->count() - 1, w);
                ++shown;
            }
        }
        emptyHint->setVisible(shown == 0);
        countTag->setText(QString::number(shown));
    };
    if (m_media) {
        connect(m_media, &MediaRegistry::changed, this, rebuildBin);
    }
    connect(search, &QLineEdit::textChanged, this, [rebuildBin](const QString&){ rebuildBin(); });
    rebuildBin();

    auto* rightSplit = new QSplitter(Qt::Horizontal);

    // ── Preview monitor + transport ──
    auto* prev = new QWidget;
    prev->setObjectName(QStringLiteral("workspaceBody"));
    auto* pv = new QVBoxLayout(prev);
    pv->setContentsMargins(16, 12, 16, 12); pv->setSpacing(10);
    auto* modeRow = new QHBoxLayout; modeRow->setSpacing(2);
    for (const QString& m : {tr("Program"), tr("Source"), tr("Multicam")}) {
        auto* b = new QPushButton(m); b->setObjectName(QStringLiteral("headerTab"));
        if (m == tr("Program")) Theme::setProp(b, "active", true);
        modeRow->addWidget(b);
    }
    modeRow->addStretch();
    modeRow->addWidget(lbl(QStringLiteral("1920 × 1080 · 60 fps"), QStringLiteral("mute"), 11, false, true));
    pv->addLayout(modeRow);
    pv->addWidget(new Placeholder(tr("Program · 00:00:00"), 16, 9), 1);
    auto* transport = new QHBoxLayout; transport->setSpacing(6);
    auto tbtn = [&](const QString& icon) {
        auto* b = new QPushButton(Icons::icon(icon, Theme::TextDim, 14), QString());
        Theme::setVariant(b, QStringLiteral("ghost")); b->setFixedWidth(30); return b;
    };
    auto* skipBackBtn = tbtn(QStringLiteral("skipBack"));
    transport->addWidget(skipBackBtn);
    auto* prevFrameBtn = tbtn(QStringLiteral("chevLeft"));
    prevFrameBtn->setToolTip(tr("Step back one frame"));
    transport->addWidget(prevFrameBtn);
    auto* play = new QPushButton(Icons::icon(QStringLiteral("play"), Theme::BgBase, 14), QString());
    Theme::setVariant(play, QStringLiteral("primary")); play->setFixedWidth(34);
    transport->addWidget(play);
    auto* nextFrameBtn = tbtn(QStringLiteral("chevRight"));
    nextFrameBtn->setToolTip(tr("Step forward one frame"));
    transport->addWidget(nextFrameBtn);
    auto* skipFwdBtn = tbtn(QStringLiteral("skipFwd"));
    transport->addWidget(skipFwdBtn);
    auto* tcLabel = lbl(QStringLiteral("00:00:00:00"), QString(), 13, true, true);
    transport->addWidget(tcLabel);
    auto* tcTotal = lbl(QStringLiteral("/ 00:06:00:00"), QStringLiteral("mute"), 11, false, true);
    transport->addWidget(tcTotal);
    transport->addStretch();
    auto* splitBtn = new QPushButton(Icons::icon(QStringLiteral("scissors"), Theme::Text, 11), tr(" Split"));
    splitBtn->setToolTip(tr("Split the selected (or playhead-overlapping) clip"));
    transport->addWidget(splitBtn);
    auto* exp = new QPushButton(Icons::icon(QStringLiteral("upload"), Theme::Text, 11), tr(" Export"));
    transport->addWidget(exp);
    pv->addLayout(transport);
    rightSplit->addWidget(prev);

    // ── Inspector ──
    auto* insp = new PanelFrame(tr("Inspector"), QStringLiteral("settings"));
    auto* clipTag = Theme::makeTag(tr("No selection"), QStringLiteral("mute"));
    insp->addHeaderWidget(clipTag);
    auto* tabs = new QTabWidget;

    // — Effects page (unchanged this increment; flagged as follow-up) —
    auto effectsPage = [&]() {
        auto* w = new QWidget; auto* v = new QVBoxLayout(w);
        v->setContentsMargins(12, 12, 12, 12); v->setSpacing(10);
        auto effectCard = [&](const QString& name, std::initializer_list<QString> rows) {
            auto* c = new QFrame; c->setObjectName(QStringLiteral("card"));
            auto* cv = new QVBoxLayout(c); cv->setContentsMargins(10, 8, 10, 8); cv->setSpacing(6);
            cv->addWidget(lbl(name, QString(), 12, true));
            for (const QString& r : rows) {
                auto* rr = new QHBoxLayout;
                rr->addWidget(lbl(r, QStringLiteral("mute"), 11)); rr->addStretch();
                auto* s = new QSlider(Qt::Horizontal); s->setFixedWidth(120); s->setValue(40);
                rr->addWidget(s);
                cv->addLayout(rr);
            }
            return c;
        };
        v->addWidget(effectCard(tr("Color Correction"), {tr("Exposure"), tr("Contrast"), tr("Saturation")}));
        v->addWidget(effectCard(tr("Sharpen"), {tr("Amount")}));
        auto* add = new QPushButton(Icons::icon(QStringLiteral("plus"), Theme::Text, 11), tr(" Add Effect"));
        v->addWidget(add, 0, Qt::AlignLeft);
        v->addStretch();
        return w;
    };

    // — Transform page —
    auto* tfPage = new QWidget;
    auto* tfFormHost = new QFormLayout(tfPage);
    tfFormHost->setContentsMargins(12, 12, 12, 12);
    tfFormHost->setFormAlignment(Qt::AlignTop | Qt::AlignLeft);
    auto* xSpin = new QSpinBox; xSpin->setRange(-10000, 10000);
    auto* ySpin = new QSpinBox; ySpin->setRange(-10000, 10000);
    auto* scaleSpin = new QDoubleSpinBox; scaleSpin->setRange(1.0, 1000.0); scaleSpin->setSuffix(QStringLiteral(" %"));
    auto* rotSpin = new QDoubleSpinBox; rotSpin->setRange(-360.0, 360.0); rotSpin->setSuffix(QStringLiteral("°")); rotSpin->setDecimals(1);
    auto* opacitySlider = new QSlider(Qt::Horizontal); opacitySlider->setRange(0, 100);
    auto* opacityVal = lbl(QStringLiteral("100 %"), QStringLiteral("mute"), 11, false, true);
    opacityVal->setFixedWidth(56);
    opacityVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto* opacityRow = new QWidget; auto* orh = new QHBoxLayout(opacityRow);
    orh->setContentsMargins(0, 0, 0, 0); orh->setSpacing(6);
    orh->addWidget(opacitySlider, 1); orh->addWidget(opacityVal);
    tfFormHost->addRow(tr("X"),        xSpin);
    tfFormHost->addRow(tr("Y"),        ySpin);
    tfFormHost->addRow(tr("Scale"),    scaleSpin);
    tfFormHost->addRow(tr("Rotation"), rotSpin);
    tfFormHost->addRow(tr("Opacity"),  opacityRow);

    // — Audio page —
    auto* audPage = new QWidget;
    auto* audForm = new QFormLayout(audPage);
    audForm->setContentsMargins(12, 12, 12, 12);
    audForm->setFormAlignment(Qt::AlignTop | Qt::AlignLeft);
    auto makeSliderRow = [](QSlider* s, QLabel* readout) {
        auto* w = new QWidget; auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0); h->setSpacing(6);
        h->addWidget(s, 1); h->addWidget(readout);
        return w;
    };
    auto* gainSlider = new QSlider(Qt::Horizontal); gainSlider->setRange(-30, 12);
    auto* gainVal = lbl(QStringLiteral("0 dB"), QStringLiteral("mute"), 11, false, true);
    gainVal->setFixedWidth(56); gainVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto* panSlider = new QSlider(Qt::Horizontal); panSlider->setRange(-100, 100);
    auto* panVal = lbl(QStringLiteral("C"), QStringLiteral("mute"), 11, false, true);
    panVal->setFixedWidth(56); panVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto* channelsCombo = new QComboBox;
    channelsCombo->addItems({tr("Stereo"), tr("Mono Left"), tr("Mono Right")});
    audForm->addRow(tr("Gain"),     makeSliderRow(gainSlider, gainVal));
    audForm->addRow(tr("Pan"),      makeSliderRow(panSlider, panVal));
    audForm->addRow(tr("Channels"), channelsCombo);

    // — Speed page —
    auto* spdPage = new QWidget;
    auto* spdLayout = new QVBoxLayout(spdPage);
    spdLayout->setContentsMargins(12, 12, 12, 12); spdLayout->setSpacing(10);
    auto* spdRow = new QHBoxLayout;
    spdRow->addWidget(lbl(tr("Speed"), QStringLiteral("mute"), 12));
    auto* spdSlider = new QSlider(Qt::Horizontal); spdSlider->setRange(10, 400);
    auto* spdVal = lbl(QStringLiteral("100 %"), QStringLiteral("mute"), 11, false, true);
    spdVal->setFixedWidth(56); spdVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    spdRow->addWidget(spdSlider, 1); spdRow->addWidget(spdVal);
    spdLayout->addLayout(spdRow);
    auto* rampBtn = new QPushButton(Icons::icon(QStringLiteral("plus"), Theme::Text, 11),
                                    tr(" Add ramp keyframe"));
    rampBtn->setToolTip(tr("Speed ramps are not yet implemented (planned)."));
    spdLayout->addWidget(rampBtn, 0, Qt::AlignLeft);
    spdLayout->addStretch();

    tabs->addTab(effectsPage(), tr("Effects"));
    tabs->addTab(tfPage,  tr("Transform"));
    tabs->addTab(audPage, tr("Audio"));
    tabs->addTab(spdPage, tr("Speed"));
    insp->bodyLayout()->addWidget(tabs);
    rightSplit->addWidget(insp);

    rightSplit->setStretchFactor(0, 3);
    rightSplit->setStretchFactor(1, 1);
    topSplit->addWidget(rightSplit);
    topSplit->setStretchFactor(0, 0);
    topSplit->setStretchFactor(1, 1);
    topSplit->setSizes({300, 1100});
    col->addWidget(topSplit, 55);

    // ── Toolbar ─────────────────────────────────────────────────────────────
    auto* toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("workspaceHeader"));
    toolbar->setFixedHeight(38);
    auto* tb = new QHBoxLayout(toolbar);
    tb->setContentsMargins(12, 0, 12, 0); tb->setSpacing(6);
    auto* toolGroup = new QButtonGroup(this);
    toolGroup->setExclusive(true);
    const struct { QString icon; QString tip; } toolDefs[] = {
        {QStringLiteral("drag"),     tr("Select")},
        {QStringLiteral("scissors"), tr("Razor — click a clip to split")},
        {QStringLiteral("refresh"),  tr("Slip")},
        {QStringLiteral("layers"),   tr("Hand")},
    };
    QVector<QPushButton*> toolBtns;
    for (const auto& td : toolDefs) {
        auto* b = new QPushButton(Icons::icon(td.icon, Theme::TextDim, 13), QString());
        Theme::setVariant(b, QStringLiteral("ghost"));
        b->setFixedWidth(28);
        b->setCheckable(true);
        b->setToolTip(td.tip);
        toolGroup->addButton(b);
        tb->addWidget(b);
        toolBtns << b;
    }
    toolBtns[0]->setChecked(true);
    auto* snapBtn = new QPushButton(Icons::icon(QStringLiteral("check"), Theme::AccentHi, 12), tr(" Snap"));
    Theme::setVariant(snapBtn, QStringLiteral("ghost"));
    snapBtn->setCheckable(true);
    snapBtn->setChecked(true);
    snapBtn->setToolTip(tr("Snap to clip edges, playhead, and the second grid"));
    tb->addWidget(snapBtn);
    tb->addStretch();
    tb->addWidget(lbl(tr("ZOOM"), QStringLiteral("mute"), 11));
    auto* zoom = new QSlider(Qt::Horizontal); zoom->setRange(10, 200); zoom->setValue(60);
    zoom->setFixedWidth(140);
    tb->addWidget(zoom);
    auto* zoomVal = lbl(QStringLiteral("60px/s"), QStringLiteral("mute"), 10, false, true);
    tb->addWidget(zoomVal);
    auto* dot = new QLabel; dot->setFixedSize(8, 8);
    dot->setStyleSheet(QStringLiteral("background:%1;border-radius:4px;").arg(Theme::Success.name()));
    tb->addWidget(dot);
    tb->addWidget(lbl(tr("Autosaved · 14s ago"), QStringLiteral("mute"), 11, false, true));
    col->addWidget(toolbar);

    // ── Bottom half: timeline ───────────────────────────────────────────────
    auto* bottom = new QWidget(this);
    auto* bh = new QHBoxLayout(bottom);
    bh->setContentsMargins(0, 0, 0, 0); bh->setSpacing(0);

    // Track headers
    auto* heads = new QWidget(bottom);
    heads->setObjectName(QStringLiteral("recSideCol"));
    heads->setFixedWidth(kHeaderW);
    auto* hv = new QVBoxLayout(heads);
    hv->setContentsMargins(0, 0, 0, 0); hv->setSpacing(0);
    hv->addSpacing(kRulerH);
    for (const Track& t : tracks()) {
        auto* w = new QWidget; w->setFixedHeight(kTrackH);
        auto* hh = new QHBoxLayout(w); hh->setContentsMargins(8, 0, 8, 0); hh->setSpacing(6);
        auto* id = lbl(t.id, t.audio ? QStringLiteral("success") : QStringLiteral("accent"), 10, true, true);
        id->setFixedWidth(20);
        hh->addWidget(id);
        hh->addWidget(lbl(t.name, QStringLiteral("dim"), 11));
        hh->addStretch();
        auto* eye = new QLabel; eye->setPixmap(Icons::pixmap(t.audio ? QStringLiteral("speaker") : QStringLiteral("eye"), Theme::TextMute, 11));
        hh->addWidget(eye);
        auto* d2 = new QFrame; d2->setObjectName(QStringLiteral("divider")); d2->setFixedHeight(1);
        auto* cellWrap = new QWidget; auto* cwv = new QVBoxLayout(cellWrap);
        cwv->setContentsMargins(0, 0, 0, 0); cwv->setSpacing(0);
        cwv->addWidget(w); cwv->addWidget(d2);
        hv->addWidget(cellWrap);
    }
    hv->addStretch();
    bh->addWidget(heads);
    auto* vdiv = new QFrame; vdiv->setObjectName(QStringLiteral("divider")); vdiv->setFixedWidth(1);
    bh->addWidget(vdiv);

    // Scrollable canvas
    auto* scroll = new QScrollArea(bottom);
    scroll->setWidgetResizable(false);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* canvas = new TimelineCanvas(scroll);
    m_timelineCanvas = canvas;
    scroll->setWidget(canvas);
    bh->addWidget(scroll, 1);

    connect(zoom, &QSlider::valueChanged, this, [canvas, zoomVal](int v) {
        canvas->setZoom(double(v));
        zoomVal->setText(QStringLiteral("%1px/s").arg(v));
    });

    connect(toolBtns[0], &QPushButton::clicked, canvas, [canvas] { canvas->setTool(TimelineCanvas::Tool::Select); });
    connect(toolBtns[1], &QPushButton::clicked, canvas, [canvas] { canvas->setTool(TimelineCanvas::Tool::Razor); });
    connect(toolBtns[2], &QPushButton::clicked, canvas, [canvas] { canvas->setTool(TimelineCanvas::Tool::Slip); });
    connect(toolBtns[3], &QPushButton::clicked, canvas, [canvas] { canvas->setTool(TimelineCanvas::Tool::Hand); });
    connect(snapBtn, &QPushButton::toggled, canvas, [canvas](bool on) { canvas->setSnap(on); });

    // ── Transport wiring ──
    connect(skipBackBtn,  &QPushButton::clicked, canvas, &TimelineCanvas::skipBack);
    connect(skipFwdBtn,   &QPushButton::clicked, canvas, &TimelineCanvas::skipForward);
    connect(prevFrameBtn, &QPushButton::clicked, canvas, [canvas]{ canvas->frameStep(-1); });
    connect(nextFrameBtn, &QPushButton::clicked, canvas, [canvas]{ canvas->frameStep(+1); });
    connect(play,         &QPushButton::clicked, canvas, &TimelineCanvas::togglePlay);
    connect(splitBtn,     &QPushButton::clicked, canvas, &TimelineCanvas::splitAtPlayhead);

    connect(canvas, &TimelineCanvas::playingChanged, play, [play](bool playing) {
        play->setIcon(Icons::icon(playing ? QStringLiteral("pause") : QStringLiteral("play"),
                                  Theme::BgBase, 14));
        play->setToolTip(playing ? tr("Pause") : tr("Play"));
    });
    connect(canvas, &TimelineCanvas::playheadChanged, tcLabel, [tcLabel](double t) {
        tcLabel->setText(formatTimecode(t));
    });

    // ── Inspector ↔ canvas selection binding ──
    auto loadInspectorFromSelection = [=, this]() {
        const bool has = canvas->selectedIndex() >= 0;
        const bool isAud = canvas->selectedIsAudio();

        // Block signals on EVERY editable widget so reading values back doesn't
        // re-enter applyField via the change handlers we hook up below.
        const QSignalBlocker b1(xSpin),       b2(ySpin),       b3(scaleSpin),
                             b4(rotSpin),     b5(opacitySlider),
                             b6(gainSlider),  b7(panSlider),   b8(channelsCombo),
                             b9(spdSlider);

        xSpin->setValue(canvas->selectedField_tx());
        ySpin->setValue(canvas->selectedField_ty());
        scaleSpin->setValue(canvas->selectedField_scale());
        rotSpin->setValue(canvas->selectedField_rotation());
        opacitySlider->setValue(canvas->selectedField_opacity());
        opacityVal->setText(QStringLiteral("%1 %").arg(opacitySlider->value()));
        gainSlider->setValue(canvas->selectedField_gainDb());
        gainVal->setText(QStringLiteral("%1 dB").arg(gainSlider->value()));
        panSlider->setValue(canvas->selectedField_pan());
        panVal->setText(panSlider->value() == 0
            ? QStringLiteral("C")
            : (panSlider->value() < 0
                ? QStringLiteral("L %1").arg(-panSlider->value())
                : QStringLiteral("R %1").arg(panSlider->value())));
        channelsCombo->setCurrentIndex(canvas->selectedField_channels());
        spdSlider->setValue(int(std::round(canvas->selectedField_speed() * 100.0)));
        spdVal->setText(QStringLiteral("%1 %").arg(spdSlider->value()));

        // Enable/disable whole pages by selection + per-track-kind for Audio.
        tfPage->setEnabled(has);
        spdPage->setEnabled(has);
        audPage->setEnabled(has && isAud);

        clipTag->setText(has ? canvas->selectedLabel() : tr("No selection"));
        Theme::setTone(clipTag,
            has ? QString(isAud ? "success" : "accent") : QStringLiteral("mute"));
    };
    connect(canvas, &TimelineCanvas::selectionChanged,
            this, [loadInspectorFromSelection](int) { loadInspectorFromSelection(); });

    // ── Inspector write-back ──
    connect(xSpin,         qOverload<int>(&QSpinBox::valueChanged),
            canvas, [canvas](int v){ canvas->setField_tx(v); });
    connect(ySpin,         qOverload<int>(&QSpinBox::valueChanged),
            canvas, [canvas](int v){ canvas->setField_ty(v); });
    connect(scaleSpin,     qOverload<double>(&QDoubleSpinBox::valueChanged),
            canvas, [canvas](double v){ canvas->setField_scale(v); });
    connect(rotSpin,       qOverload<double>(&QDoubleSpinBox::valueChanged),
            canvas, [canvas](double v){ canvas->setField_rotation(v); });
    connect(opacitySlider, &QSlider::valueChanged,
            this, [canvas, opacityVal](int v){
                canvas->setField_opacity(v);
                opacityVal->setText(QStringLiteral("%1 %").arg(v));
            });
    connect(gainSlider,    &QSlider::valueChanged,
            this, [canvas, gainVal](int v){
                canvas->setField_gainDb(v);
                gainVal->setText(QStringLiteral("%1 dB").arg(v));
            });
    connect(panSlider,     &QSlider::valueChanged,
            this, [canvas, panVal](int v){
                canvas->setField_pan(v);
                panVal->setText(v == 0
                    ? QStringLiteral("C")
                    : (v < 0 ? QStringLiteral("L %1").arg(-v) : QStringLiteral("R %1").arg(v)));
            });
    connect(channelsCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            canvas, [canvas](int v){ canvas->setField_channels(v); });
    connect(spdSlider,     &QSlider::valueChanged,
            this, [canvas, spdVal](int v){
                canvas->setField_speed(double(v) / 100.0);
                spdVal->setText(QStringLiteral("%1 %").arg(v));
            });

    // Initial state: no selection.
    loadInspectorFromSelection();

    col->addWidget(bottom, 45);
}

QJsonArray EditorWorkspace::timelineJson() const {
    return m_timelineCanvas
        ? static_cast<TimelineCanvas*>(m_timelineCanvas)->toJson()
        : QJsonArray{};
}

void EditorWorkspace::setTimelineJson(const QJsonArray& timeline) {
    if (m_timelineCanvas)
        static_cast<TimelineCanvas*>(m_timelineCanvas)->setClips(timeline);
}

// AUTOMOC: this .cpp declares Q_OBJECT classes (TimelineCanvas), so the moc'd
// meta-object code must be included into this TU.
#include "EditorWorkspace.moc"
