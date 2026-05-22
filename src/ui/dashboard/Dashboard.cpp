#include "ui/dashboard/Dashboard.h"
#include "ui/components/MeterBar.h"
#include "ui/components/PanelFrame.h"
#include "ui/components/Placeholder.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"
#include "recording/RenderQueue.h"

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace {

QFrame* card(QWidget* parent = nullptr) {
    auto* f = new QFrame(parent);
    f->setObjectName(QStringLiteral("card"));
    return f;
}

QLabel* mono(const QString& text, const QString& tone = QStringLiteral("mute"), int px = 11) {
    return Theme::label(text, tone, px, false, true);
}

QLabel* text(const QString& s, const QString& tone = QString(), int px = 13, bool bold = false) {
    return Theme::label(s, tone, px, bold);
}

QFrame* iconChip(const QString& name, const QColor& color, int chip = 32, int ic = 16) {
    auto* f = new QFrame;
    f->setObjectName(QStringLiteral("iconChip"));
    f->setFixedSize(chip, chip);
    auto* v = new QVBoxLayout(f);
    v->setContentsMargins(0, 0, 0, 0);
    auto* l = new QLabel(f);
    l->setAlignment(Qt::AlignCenter);
    l->setPixmap(Icons::pixmap(name, color, ic));
    v->addWidget(l);
    return f;
}

QWidget* factPill(const QString& label, const QString& value) {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    auto* l = new QLabel(label.toUpper());
    l->setProperty("tone", "mute");
    QFont lf = l->font(); lf.setPixelSize(10); lf.setLetterSpacing(QFont::AbsoluteSpacing, 0.6); l->setFont(lf);
    v->addWidget(l);
    v->addWidget(mono(value, QString(), 12));
    return w;
}

QString dbText(double v) {
    if (v < 0.001) return QStringLiteral("-∞ dB");
    return QStringLiteral("%1 dB").arg(20.0 * std::log10(v), 0, 'f', 1);
}

} // namespace

Dashboard::Dashboard(RenderQueue* renderQueue, QWidget* parent)
    : QWidget(parent), m_renderQueue(renderQueue) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("workspaceBody"));
    auto* col = new QVBoxLayout(content);
    col->setContentsMargins(32, 24, 32, 32);
    col->setSpacing(20);

    // Row 1: hero (1.4) + quick actions (1)
    auto* r1 = new QHBoxLayout;
    r1->setSpacing(20);
    r1->addWidget(buildHero(), 14);
    r1->addWidget(buildQuickActions(), 10);
    col->addLayout(r1);

    // Row 2: now playing | recent recordings | render queue
    auto* r2 = new QHBoxLayout;
    r2->setSpacing(20);
    r2->addWidget(buildNowPlaying(), 1);
    r2->addWidget(buildRecentRecordings(), 1);
    r2->addWidget(buildRenderQueue(), 1);
    col->addLayout(r2);

    // Row 3: recent projects (1.3) + recent clips (1)
    auto* r3 = new QHBoxLayout;
    r3->setSpacing(20);
    r3->addWidget(buildRecentProjects(), 13);
    r3->addWidget(buildRecentClips(), 10);
    col->addLayout(r3);

    // Row 4: system status (full width)
    col->addWidget(buildSystemStatus());
    col->addStretch();

    scroll->setWidget(content);

    m_meterTimer = new QTimer(this);
    m_meterTimer->setInterval(60);
    connect(m_meterTimer, &QTimer::timeout, this, &Dashboard::tickMeters);
    m_meterTimer->start();

    if (m_renderQueue)
        connect(m_renderQueue, &RenderQueue::changed, this, &Dashboard::refreshRenderQueue);
}

void Dashboard::setProjectName(const QString& name) {
    if (m_heroTitle) m_heroTitle->setText(name.isEmpty() ? tr("Untitled project") : name);
}

void Dashboard::setRecording(bool on) { m_recording = on; refreshState(); }
void Dashboard::setStreaming(bool on) { m_streaming = on; refreshState(); }

void Dashboard::refreshState() {
    if (m_recBtn) {
        m_recBtn->setText(m_recording ? tr("  Stop Recording") : tr("  Start Recording"));
        m_recBtn->setIcon(Icons::icon(m_recording ? QStringLiteral("stop") : QStringLiteral("record"),
                                      Theme::Text, 14));
    }
    if (m_liveBtn) {
        m_liveBtn->setText(m_streaming ? tr("  End Stream") : tr("  Go Live"));
        Theme::setVariant(m_liveBtn, m_streaming ? QStringLiteral("outlineRec") : QString());
    }
}

QWidget* Dashboard::buildHero() {
    auto* c = card();
    auto* v = new QVBoxLayout(c);
    v->setContentsMargins(24, 24, 24, 24);
    v->setSpacing(12);

    auto* top = new QHBoxLayout;
    top->setSpacing(8);
    top->addWidget(Theme::makeTag(tr("Project"), QStringLiteral("accent")));
    top->addWidget(text(tr("Stream Night · Tuesday · 2 hr planned"), QStringLiteral("mute"), 12));
    top->addStretch();
    v->addLayout(top);

    m_heroTitle = new QLabel(tr("Untitled project"));
    m_heroTitle->setObjectName(QStringLiteral("heroTitle"));
    v->addWidget(m_heroTitle);

    auto* desc = text(tr("Compose scenes, record and stream, then clip and edit — all in one workstation."),
                      QStringLiteral("dim"), 13);
    desc->setWordWrap(true);
    desc->setMaximumWidth(560);
    v->addWidget(desc);

    auto* btns = new QHBoxLayout;
    btns->setSpacing(12);
    m_recBtn = new QPushButton(Icons::icon(QStringLiteral("record"), Theme::Text, 14), tr("  Start Recording"));
    Theme::setVariant(m_recBtn, QStringLiteral("rec"));
    m_recBtn->setCursor(Qt::PointingHandCursor);
    connect(m_recBtn, &QPushButton::clicked, this, &Dashboard::recordRequested);
    m_liveBtn = new QPushButton(Icons::icon(QStringLiteral("stream"), Theme::Text, 14), tr("  Go Live"));
    m_liveBtn->setCursor(Qt::PointingHandCursor);
    connect(m_liveBtn, &QPushButton::clicked, this, &Dashboard::streamRequested);
    auto* edit = new QPushButton(Icons::icon(QStringLiteral("editor"), Theme::TextDim, 14), tr("  Open Editor"));
    Theme::setVariant(edit, QStringLiteral("ghost"));
    edit->setCursor(Qt::PointingHandCursor);
    connect(edit, &QPushButton::clicked, this, [this] { emit navigateTo(QStringLiteral("editor")); });
    btns->addWidget(m_recBtn);
    btns->addWidget(m_liveBtn);
    btns->addWidget(edit);
    btns->addStretch();
    v->addLayout(btns);

    auto* facts = new QHBoxLayout;
    facts->setSpacing(24);
    facts->addWidget(factPill(tr("Resolution"), QStringLiteral("1920 × 1080")));
    facts->addWidget(factPill(tr("Framerate"), QStringLiteral("60 fps")));
    facts->addWidget(factPill(tr("Audio"), QStringLiteral("48 kHz · Stereo")));
    facts->addWidget(factPill(tr("Mic"), QStringLiteral("Shure SM7B")));
    facts->addWidget(factPill(tr("Destination"), QStringLiteral("Twitch · /malloy_live")));
    facts->addStretch();
    v->addLayout(facts);
    v->addStretch();

    return c;
}

QWidget* Dashboard::buildQuickActions() {
    auto* c = card();
    auto* v = new QVBoxLayout(c);
    v->setContentsMargins(16, 12, 16, 16);
    v->setSpacing(8);
    v->addWidget(Theme::makeSectionHeader(tr("Quick actions")));

    auto* grid = new QGridLayout;
    grid->setSpacing(8);
    struct A { QString icon, label, sub, target; QColor color; };
    const QVector<A> items = {
        {QStringLiteral("record"),   tr("Start Recording"), tr("Use last profile · F9"), QStringLiteral("record"), Theme::RecHi},
        {QStringLiteral("stream"),   tr("Go Live"),          tr("Twitch · Stream Setup"), QStringLiteral("stream"), Theme::AccentHi},
        {QStringLiteral("editor"),   tr("Open Editor"),      tr("Resume highlights"),      QStringLiteral("editor"), Theme::TextDim},
        {QStringLiteral("upload"),   tr("Import Media"),     tr("Drag files or browse"),   QStringLiteral("media"),  Theme::TextDim},
        {QStringLiteral("scissors"), tr("Clip last 30 s"),   tr("Replay buffer"),          QStringLiteral("clips"),  Theme::TextDim},
        {QStringLiteral("projects"), tr("New Project"),      tr("From scratch or template"), QStringLiteral("projects"), Theme::TextDim},
    };
    int i = 0;
    for (const A& a : items) {
        auto* b = new QPushButton;
        b->setObjectName(QStringLiteral("actionButton"));
        b->setCursor(Qt::PointingHandCursor);
        auto* h = new QHBoxLayout(b);
        h->setContentsMargins(8, 8, 8, 8);
        h->setSpacing(12);
        h->addWidget(iconChip(a.icon, a.color, 32, 16));
        auto* tv = new QVBoxLayout;
        tv->setSpacing(0);
        tv->addWidget(text(a.label, QString(), 13, true));
        tv->addWidget(text(a.sub, QStringLiteral("mute"), 11));
        h->addLayout(tv);
        h->addStretch();
        const QString target = a.target;
        connect(b, &QPushButton::clicked, this, [this, target] {
            if (target == QLatin1String("record")) emit recordRequested();
            else if (target == QLatin1String("stream")) emit streamRequested();
            else emit navigateTo(target);
        });
        grid->addWidget(b, i / 2, i % 2);
        ++i;
    }
    v->addLayout(grid);
    v->addStretch();
    return c;
}

QWidget* Dashboard::buildNowPlaying() {
    auto* panel = new PanelFrame(tr("Preview signal"), QStringLiteral("display"));
    panel->addHeaderWidget(mono(QStringLiteral("1920 × 1080 · 60 fps")));

    auto* body = new QWidget;
    auto* v = new QVBoxLayout(body);
    v->setContentsMargins(12, 12, 12, 12);
    v->setSpacing(12);
    v->addWidget(new Placeholder(tr("Scene · Gameplay"), 16, 9));

    auto addMeterRow = [&](const QString& label, MeterBar** meterOut, QLabel** dbOut) {
        auto* row = new QHBoxLayout;
        row->setSpacing(8);
        auto* l = text(label, QStringLiteral("dim"), 11);
        l->setFixedWidth(110);
        row->addWidget(l);
        auto* m = new MeterBar;
        row->addWidget(m, 1);
        auto* db = mono(QStringLiteral("-∞ dB"), QStringLiteral("dim"));
        db->setFixedWidth(56);
        db->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(db);
        v->addLayout(row);
        if (meterOut) *meterOut = m;
        if (dbOut) *dbOut = db;
    };
    addMeterRow(tr("Mic — SM7B"), &m_micMeter, &m_micDb);
    addMeterRow(tr("Desktop Audio"), &m_deskMeter, &m_deskDb);
    v->addStretch();

    panel->bodyLayout()->addWidget(body);
    return panel;
}

QWidget* Dashboard::buildRecentRecordings() {
    auto* panel = new PanelFrame(tr("Recent recordings"), QStringLiteral("record"));
    auto* viewAll = new QPushButton(tr("View all"));
    Theme::setVariant(viewAll, QStringLiteral("ghost"));
    panel->addHeaderWidget(viewAll);

    auto* body = new QWidget;
    auto* v = new QVBoxLayout(body);
    v->setContentsMargins(6, 4, 6, 6);
    v->setSpacing(2);
    struct R { QString name, meta, when; bool warn; };
    const QVector<R> recs = {
        {tr("Spire — Ep 14 raw.mkv"), QStringLiteral("01:47:21 · 14.2 GB"), tr("Today, 1h ago"), false},
        {tr("Spire — Ep 13 raw.mkv"), QStringLiteral("02:11:08 · 17.8 GB"), tr("Mon · 8:14 PM"), false},
        {tr("Late test stream.mkv"),  QStringLiteral("00:23:55 · 3.0 GB"),  tr("Sun · 11:02 PM"), true},
        {tr("Voice memo — intro.mp3"),QStringLiteral("00:01:12 · 1.1 MB"),  tr("Sun · 9:45 PM"), false},
    };
    for (const R& r : recs) {
        auto* row = new QWidget;
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(6, 4, 6, 4);
        h->setSpacing(8);
        h->addWidget(iconChip(QStringLiteral("record"), Theme::TextMute, 26, 12));
        auto* tv = new QVBoxLayout; tv->setSpacing(0);
        tv->addWidget(text(r.name, QString(), 12));
        tv->addWidget(mono(r.meta, QStringLiteral("mute"), 10));
        h->addLayout(tv);
        h->addStretch();
        h->addWidget(text(r.when, QStringLiteral("mute"), 11));
        if (r.warn) h->addWidget(Theme::makeTag(tr("truncated"), QStringLiteral("warn")));
        v->addWidget(row);
    }
    v->addStretch();
    panel->bodyLayout()->addWidget(body);
    return panel;
}

QWidget* Dashboard::buildRenderQueue() {
    auto* panel = new PanelFrame(tr("Render queue"), QStringLiteral("render"));
    m_renderMeta = mono(QString());
    panel->addHeaderWidget(m_renderMeta);

    auto* body = new QWidget;
    m_renderBodyLayout = new QVBoxLayout(body);
    m_renderBodyLayout->setContentsMargins(12, 12, 12, 12);
    m_renderBodyLayout->setSpacing(12);
    panel->bodyLayout()->addWidget(body);

    refreshRenderQueue();   // populate from the live queue (or empty state)
    return panel;
}

void Dashboard::refreshRenderQueue() {
    if (!m_renderBodyLayout) return;
    // Clear previous content.
    while (QLayoutItem* it = m_renderBodyLayout->takeAt(0)) {
        if (it->widget()) it->widget()->deleteLater();
        delete it;
    }

    using S = RenderJob::State;
    const QVector<RenderJob> all = m_renderQueue ? m_renderQueue->jobs() : QVector<RenderJob>{};
    if (m_renderMeta && m_renderQueue) {
        m_renderMeta->setText(tr("%1 active · %2 pending")
            .arg(m_renderQueue->countOfState(S::Active))
            .arg(m_renderQueue->countOfState(S::Pending)));
    }

    // Show active + pending jobs (up to 4), newest-relevant first.
    int shown = 0;
    auto addJob = [&](const RenderJob& j, bool active) {
        auto* w = new QWidget;
        auto* jv = new QVBoxLayout(w);
        jv->setContentsMargins(0, 0, 0, 0);
        jv->setSpacing(4);
        auto* hr = new QHBoxLayout;
        hr->addWidget(text(j.name, QString(), 12));
        hr->addStretch();
        hr->addWidget(mono(active ? tr("%1%").arg(j.progress) : tr("queued")));
        jv->addLayout(hr);
        auto* bar = new QProgressBar;
        bar->setRange(0, 100);
        bar->setValue(j.progress);
        bar->setTextVisible(false);
        bar->setFixedHeight(4);
        if (!active) bar->setProperty("tone", "idle");
        jv->addWidget(bar);
        m_renderBodyLayout->addWidget(w);
    };
    for (const RenderJob& j : all) { if (j.state == S::Active)  { addJob(j, true);  ++shown; } }
    for (const RenderJob& j : all) { if (shown >= 4) break; if (j.state == S::Pending) { addJob(j, false); ++shown; } }

    if (shown == 0) {
        auto* empty = text(tr("No renders queued — start one from the Render Queue."),
                           QStringLiteral("mute"), 12);
        empty->setWordWrap(true);
        m_renderBodyLayout->addWidget(empty);
    }
    m_renderBodyLayout->addStretch();
}

QWidget* Dashboard::buildRecentProjects() {
    auto* panel = new PanelFrame(tr("Recent projects"), QStringLiteral("projects"));
    auto* neu = new QPushButton(Icons::icon(QStringLiteral("plus"), Theme::Text, 12), tr(" New"));
    connect(neu, &QPushButton::clicked, this, [this] { emit navigateTo(QStringLiteral("projects")); });
    panel->addHeaderWidget(neu);

    auto* body = new QWidget;
    auto* grid = new QGridLayout(body);
    grid->setContentsMargins(12, 12, 12, 12);
    grid->setSpacing(12);
    struct P { QString cover, name, meta; };
    const QVector<P> projects = {
        {tr("Scene: gameplay"), tr("Spire of the Hollow Sun"), QStringLiteral("2 hr ago · 142 GB")},
        {tr("Webcam · A-roll"), tr("Tuesday Vlog — Week 22"),  QStringLiteral("Yesterday · 38 GB")},
        {tr("Editor capture"),  tr("Coding Sessions · S3"),    QStringLiteral("4 days · 88 GB")},
        {tr("Final cut · v3"),  tr("Boss Rush — Compilation"), QStringLiteral("2 weeks · 21 GB")},
    };
    int i = 0;
    for (const P& p : projects) {
        auto* cell = new QWidget;
        auto* cv = new QVBoxLayout(cell);
        cv->setContentsMargins(0, 0, 0, 0);
        cv->setSpacing(6);
        cv->addWidget(new Placeholder(p.cover, 16, 10));
        cv->addWidget(text(p.name, QString(), 12, true));
        cv->addWidget(mono(p.meta, QStringLiteral("mute"), 10));
        grid->addWidget(cell, 0, i++);
    }
    panel->bodyLayout()->addWidget(body);
    return panel;
}

QWidget* Dashboard::buildRecentClips() {
    auto* panel = new PanelFrame(tr("Recent clips"), QStringLiteral("clips"));
    auto* lib = new QPushButton(tr("Clips library"));
    Theme::setVariant(lib, QStringLiteral("ghost"));
    connect(lib, &QPushButton::clicked, this, [this] { emit navigateTo(QStringLiteral("clips")); });
    panel->addHeaderWidget(lib);

    auto* body = new QWidget;
    auto* grid = new QGridLayout(body);
    grid->setContentsMargins(12, 12, 12, 12);
    grid->setSpacing(12);
    struct C { QString label, tag, dur; };
    const QVector<C> clips = {
        {tr("No-hit boss phase 3"), QStringLiteral("Spire"),  QStringLiteral("0:24")},
        {tr("Dodge into riposte"),  QStringLiteral("Spire"),  QStringLiteral("0:18")},
        {tr("Bit about coffee"),    QStringLiteral("Vlog"),   QStringLiteral("0:09")},
        {tr("Refactor reveal"),     QStringLiteral("Coding"), QStringLiteral("0:42")},
    };
    int i = 0;
    for (const C& c : clips) {
        auto* cell = new QWidget;
        auto* cv = new QVBoxLayout(cell);
        cv->setContentsMargins(0, 0, 0, 0);
        cv->setSpacing(6);
        cv->addWidget(new Placeholder(c.label, 16, 9));
        auto* meta = new QHBoxLayout;
        meta->setSpacing(6);
        meta->addWidget(Theme::makeTag(c.tag));
        meta->addStretch();
        meta->addWidget(mono(c.dur, QStringLiteral("dim")));
        cv->addLayout(meta);
        grid->addWidget(cell, i / 2, i % 2);
        ++i;
    }
    panel->bodyLayout()->addWidget(body);
    return panel;
}

QWidget* Dashboard::buildSystemStatus() {
    auto* panel = new PanelFrame(tr("System status"), QStringLiteral("info"));
    panel->addHeaderWidget(text(tr("All systems ready · checked just now"), QStringLiteral("mute"), 11));

    auto* body = new QWidget;
    auto* grid = new QGridLayout(body);
    grid->setContentsMargins(12, 12, 12, 12);
    grid->setSpacing(8);
    struct S { QString icon, label, value; };
    const QVector<S> checks = {
        {QStringLiteral("mic"),     tr("Microphone"),      QStringLiteral("Shure SM7B (USB)")},
        {QStringLiteral("camera"),  tr("Camera"),          QStringLiteral("Sony α7C · 1080p60")},
        {QStringLiteral("display"), tr("Display capture"), QStringLiteral("DXGI · Monitor 2")},
        {QStringLiteral("window"),  tr("Window capture"),  QStringLiteral("Spire of the Hollow Sun")},
        {QStringLiteral("speaker"), tr("Desktop audio"),   QStringLiteral("WASAPI loopback · 48k")},
        {QStringLiteral("cpu"),     tr("Encoder"),         QStringLiteral("NVENC H.264 ready")},
        {QStringLiteral("disk"),    tr("Storage"),         QStringLiteral("412 GB free · D:\\")},
        {QStringLiteral("link"),    tr("Stream key"),      QStringLiteral("Twitch · verified")},
    };
    int i = 0;
    for (const S& s : checks) {
        auto* item = new QFrame;
        item->setObjectName(QStringLiteral("statusItem"));
        auto* h = new QHBoxLayout(item);
        h->setContentsMargins(12, 10, 12, 10);
        h->setSpacing(12);
        h->addWidget(iconChip(s.icon, Theme::TextDim, 28, 14));
        auto* tv = new QVBoxLayout; tv->setSpacing(0);
        auto* lab = text(s.label.toUpper(), QStringLiteral("mute"), 11);
        QFont lf = lab->font(); lf.setLetterSpacing(QFont::AbsoluteSpacing, 0.5); lab->setFont(lf);
        tv->addWidget(lab);
        tv->addWidget(mono(s.value, QString(), 12));
        h->addLayout(tv);
        h->addStretch();
        auto* ok = new QLabel;
        ok->setPixmap(Icons::pixmap(QStringLiteral("check"), Theme::Success, 14));
        h->addWidget(ok);
        grid->addWidget(item, i / 4, i % 4);
        ++i;
    }
    panel->bodyLayout()->addWidget(body);
    return panel;
}

void Dashboard::tickMeters() {
    auto* rng = QRandomGenerator::global();
    auto step = [&](double& level, double& peak, double base, double variance) {
        const double target = qBound(0.05, base + (rng->generateDouble() - 0.5) * variance * 2, 0.95);
        level += (target - level) * 0.35;
        if (level > peak) peak = level;
        else peak = qMax(level, peak - 0.01);
    };
    step(m_micL, m_micP, 0.40, 0.25);
    step(m_deskL, m_deskP, 0.55, 0.20);
    if (m_micMeter) m_micMeter->setValues(m_micL, m_micP);
    if (m_deskMeter) m_deskMeter->setValues(m_deskL, m_deskP);
    if (m_micDb) m_micDb->setText(dbText(m_micP));
    if (m_deskDb) m_deskDb->setText(dbText(m_deskP));
}
