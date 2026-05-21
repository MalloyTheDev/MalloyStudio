#include "ui/workspaces/EditorWorkspace.h"
#include "ui/components/PanelFrame.h"
#include "ui/components/Placeholder.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"

#include <QButtonGroup>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSlider>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabWidget>
#include <QVBoxLayout>

#include <cmath>

namespace {

constexpr int kRulerH = 26;
constexpr int kTrackH = 44;
constexpr int kHeaderW = 200;
constexpr int kTimelineLen = 360;  // seconds
constexpr int kEdgePx = 6;         // clip trim grip width

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

struct Clip { int track; double start; double dur; QString label; QString tag; QColor color; bool audio; };
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
    o.insert(QStringLiteral("dur"), c.dur);
    o.insert(QStringLiteral("label"), c.label);
    o.insert(QStringLiteral("tag"), c.tag);
    o.insert(QStringLiteral("color"), c.color.name());
    o.insert(QStringLiteral("audio"), c.audio);
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
    return c;
}
QString timelineStorePath() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dir).filePath(QStringLiteral("editor_timeline.json"));
}

struct Marker { double t; QColor color; };
const QVector<Marker>& markers() {
    static const QVector<Marker> m = {
        {60, Theme::Accent}, {140, Theme::Warn}, {200, Theme::Rec},
    };
    return m;
}

QLabel* lbl(const QString& s, const QString& tone = QString(), int px = 13, bool bold = false, bool mono = false) {
    auto* l = new QLabel(s);
    if (!tone.isEmpty()) l->setProperty("tone", tone);
    if (mono) l->setProperty("mono", true);
    QFont f = l->font(); f.setPixelSize(px); if (bold) f.setWeight(QFont::DemiBold);
    l->setFont(f);
    return l;
}

// ── Custom-painted timeline canvas ──────────────────────────────────────────
class TimelineCanvas : public QWidget {
public:
    explicit TimelineCanvas(QWidget* parent = nullptr) : QWidget(parent) {
        setMouseTracking(true);
        loadClips();   // persisted edits, or the seed timeline on first run
        updateSize();
    }
    void setZoom(double z) { m_zoom = z; updateSize(); update(); }

    enum class Tool { Select, Razor, Slip, Hand };
    void setTool(Tool t) {
        m_tool = t;
        setCursor(t == Tool::Razor ? Qt::SplitHCursor : Qt::ArrowCursor);
    }
    void setSnap(bool on) { m_snap = on; }

protected:
    QSize sizeHint() const override { return minimumSize(); }

    void paintEvent(QPaintEvent*) override {
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

    void mousePressEvent(QMouseEvent* e) override {
        const QPointF pos = e->position();
        if (pos.y() < kRulerH) {                    // ruler → scrub playhead
            m_scrubbing = true;
            m_playhead = qBound(0.0, pos.x() / m_zoom, double(kTimelineLen));
            update();
            return;
        }
        Zone zone = Zone::None;
        const int i = clipAt(pos, &zone);
        if (m_tool == Tool::Razor) {                // razor → split at click
            if (i >= 0) splitClip(i, pos.x() / m_zoom);
            update();
            return;
        }
        m_selected = i;
        if (i >= 0) {                               // select → begin move/trim
            m_drag = i;
            m_dragZone = zone;
            m_origStart = m_clips[i].start;
            m_origDur = m_clips[i].dur;
            m_grabOffset = pos.x() / m_zoom - m_clips[i].start;
        }
        update();
    }

    void mouseMoveEvent(QMouseEvent* e) override {
        const QPointF pos = e->position();
        if (m_scrubbing) {
            m_playhead = qBound(0.0, pos.x() / m_zoom, double(kTimelineLen));
            update();
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
                // Vertical drag moves the clip between tracks of the same kind.
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
            return;
        }
        if (e->buttons() == Qt::NoButton) updateHoverCursor(pos);
    }

    void mouseReleaseEvent(QMouseEvent*) override {
        m_scrubbing = false;
        if (m_drag >= 0) saveClips();   // persist a completed move/trim
        m_drag = -1;
    }

private:
    enum class Zone { None, Body, LeftEdge, RightEdge };

    void updateSize() {
        setMinimumSize(int(kTimelineLen * m_zoom), kRulerH + tracks().size() * kTrackH);
    }

    int clipAt(const QPointF& pos, Zone* zone) const {
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

    void updateHoverCursor(const QPointF& pos) {
        if (pos.y() < kRulerH) { setCursor(Qt::SizeHorCursor); return; }
        if (m_tool == Tool::Razor) { setCursor(Qt::SplitHCursor); return; }
        Zone z = Zone::None;
        const int i = clipAt(pos, &z);
        if (i < 0) { setCursor(Qt::ArrowCursor); return; }
        setCursor(z == Zone::LeftEdge || z == Zone::RightEdge ? Qt::SizeHorCursor
                                                              : Qt::OpenHandCursor);
    }

    // Snap a time to the 1-second grid, the playhead, or any other clip's edge,
    // whichever is within ~8px. ignoreClip excludes the clip being edited.
    double snapTime(double t, int ignoreClip) const {
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

    void splitClip(int i, double t) {
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
        saveClips();
    }

    void loadClips() {
        QFile f(timelineStorePath());
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).array();
            if (!arr.isEmpty()) {
                m_clips.clear();
                for (const auto& v : arr) m_clips.push_back(clipFromJson(v.toObject()));
                return;
            }
        }
        m_clips = clips();   // first run: start from the seed timeline
    }

    void saveClips() const {
        const QString path = timelineStorePath();
        QDir().mkpath(QFileInfo(path).absolutePath());
        QJsonArray arr;
        for (const Clip& c : m_clips) arr.append(clipToJson(c));
        QSaveFile f(path);
        if (!f.open(QIODevice::WriteOnly)) return;
        f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
        f.commit();
    }

    double m_zoom = 60.0;     // px/sec
    double m_playhead = 34.0; // sec
    int m_selected = 6;       // Spire phase 1 selected by default
    bool m_scrubbing = false;

    QVector<Clip> m_clips;
    Tool m_tool = Tool::Select;
    bool m_snap = true;
    int m_drag = -1;
    Zone m_dragZone = Zone::None;
    double m_origStart = 0, m_origDur = 0, m_grabOffset = 0;
};

QFrame* iconChip(const QString& name, const QColor& color, int chip, int ic) {
    auto* f = new QFrame; f->setObjectName(QStringLiteral("iconChip")); f->setFixedSize(chip, chip);
    auto* v = new QVBoxLayout(f); v->setContentsMargins(0, 0, 0, 0);
    auto* l = new QLabel; l->setAlignment(Qt::AlignCenter); l->setPixmap(Icons::pixmap(name, color, ic));
    v->addWidget(l);
    return f;
}

} // namespace

EditorWorkspace::EditorWorkspace(QWidget* parent) : QWidget(parent) {
    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    // ── Top half: media bin | preview | inspector ──────────────────────────
    auto* topSplit = new QSplitter(Qt::Horizontal, this);

    // Media bin
    auto* bin = new PanelFrame(tr("Media Bin"), QStringLiteral("media"));
    auto* binBody = new QWidget;
    auto* bv = new QVBoxLayout(binBody);
    bv->setContentsMargins(6, 6, 6, 6); bv->setSpacing(2);
    struct B { QString name, meta, kind; };
    const QVector<B> media = {
        {tr("Spire — Ep 14 raw.mkv"), QStringLiteral("01:47:21 · 1080p60"), QStringLiteral("editor")},
        {tr("Webcam α7C — Ep 14.mkv"), QStringLiteral("01:47:18 · 1080p60"), QStringLiteral("editor")},
        {tr("Mic — SM7B (cleaned).wav"), QStringLiteral("01:47:08 · 48k"), QStringLiteral("speaker")},
        {tr("Music — Synth bed.flac"), QStringLiteral("03:42:00 · 48k"), QStringLiteral("speaker")},
        {tr("Intro card v3.mp4"), QStringLiteral("00:00:08 · 4k60"), QStringLiteral("editor")},
        {tr("Lower-third — title.png"), QStringLiteral("1920 × 240"), QStringLiteral("image")},
        {tr("B-roll — sunset.mp4"), QStringLiteral("00:00:32 · 4k30"), QStringLiteral("editor")},
    };
    for (const B& m : media) {
        auto* w = new QWidget; w->setObjectName(QStringLiteral("row")); w->setFixedHeight(36);
        auto* h = new QHBoxLayout(w); h->setContentsMargins(8, 0, 8, 0); h->setSpacing(8);
        h->addWidget(iconChip(m.kind, Theme::TextMute, 24, 11));
        auto* tv = new QVBoxLayout; tv->setSpacing(0);
        tv->addWidget(lbl(m.name, QString(), 12));
        tv->addWidget(lbl(m.meta, QStringLiteral("mute"), 10, false, true));
        h->addLayout(tv); h->addStretch();
        bv->addWidget(w);
    }
    bv->addStretch();
    bin->bodyLayout()->addWidget(binBody);
    topSplit->addWidget(bin);

    auto* rightSplit = new QSplitter(Qt::Horizontal);

    // Preview monitor
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
    pv->addWidget(new Placeholder(tr("Program · 00:34:08"), 16, 9), 1);
    auto* transport = new QHBoxLayout; transport->setSpacing(6);
    auto tbtn = [&](const QString& icon) {
        auto* b = new QPushButton(Icons::icon(icon, Theme::TextDim, 14), QString());
        Theme::setVariant(b, QStringLiteral("ghost")); b->setFixedWidth(30); return b;
    };
    transport->addWidget(tbtn(QStringLiteral("skipBack")));
    auto* play = new QPushButton(Icons::icon(QStringLiteral("play"), Theme::BgBase, 14), QString());
    Theme::setVariant(play, QStringLiteral("primary")); play->setFixedWidth(34);
    transport->addWidget(play);
    transport->addWidget(tbtn(QStringLiteral("skipFwd")));
    transport->addWidget(lbl(QStringLiteral("00:00:34:08"), QString(), 13, true, true));
    transport->addWidget(lbl(QStringLiteral("/ 00:06:00:00"), QStringLiteral("mute"), 11, false, true));
    transport->addStretch();
    transport->addWidget(new QPushButton(Icons::icon(QStringLiteral("scissors"), Theme::Text, 11), tr(" Split")));
    auto* exp = new QPushButton(Icons::icon(QStringLiteral("upload"), Theme::Text, 11), tr(" Export"));
    transport->addWidget(exp);
    pv->addLayout(transport);
    rightSplit->addWidget(prev);

    // Inspector
    auto* insp = new PanelFrame(tr("Inspector"), QStringLiteral("settings"));
    insp->addHeaderWidget(Theme::makeTag(tr("Webcam clip"), QStringLiteral("accent")));
    auto* tabs = new QTabWidget;
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
    auto simplePage = [&](const QString& text) {
        auto* w = new QWidget; auto* v = new QVBoxLayout(w);
        v->setContentsMargins(12, 12, 12, 12);
        v->addWidget(lbl(text, QStringLiteral("mute"), 12)); v->addStretch();
        return w;
    };
    tabs->addTab(effectsPage(), tr("Effects"));
    tabs->addTab(simplePage(tr("Position, scale, rotation, opacity.")), tr("Transform"));
    tabs->addTab(simplePage(tr("Gain, pan, channels.")), tr("Audio"));
    tabs->addTab(simplePage(tr("Speed and time remapping.")), tr("Speed"));
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
        auto* divb = new QFrame; divb->setObjectName(QStringLiteral("divider"));
        auto* d2 = new QFrame; d2->setObjectName(QStringLiteral("divider")); d2->setFixedHeight(1);
        auto* cellWrap = new QWidget; auto* cwv = new QVBoxLayout(cellWrap);
        cwv->setContentsMargins(0, 0, 0, 0); cwv->setSpacing(0);
        cwv->addWidget(w); cwv->addWidget(d2);
        hv->addWidget(cellWrap);
        Q_UNUSED(divb);
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

    col->addWidget(bottom, 45);
}
