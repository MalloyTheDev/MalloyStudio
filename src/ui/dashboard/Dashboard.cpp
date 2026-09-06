#include "ui/dashboard/Dashboard.h"

#include "audio/AudioController.h"
#include "model/Scene.h"
#include "model/SceneCollection.h"
#include "model/Source.h"
#include "project/ByteSize.h"
#include "project/ClipsRegistry.h"
#include "project/ProjectRegistry.h"
#include "project/RecentRecordings.h"
#include "recording/EncoderRegistry.h"
#include "recording/OutputSettings.h"
#include "recording/RenderQueue.h"
#include "recording/StreamSettings.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"
#include "ui/components/MeterBar.h"
#include "ui/components/PanelFrame.h"
#include "ui/components/Placeholder.h"

#include <QDir>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QStorageInfo>
#include <QVBoxLayout>

#include <cmath>

namespace {

// How many rows each "recent" panel shows.
constexpr int kRecentLimit = 4;
// Peak-hold falloff per mixer tick (AudioController emits levels at 50 Hz).
constexpr double kPeakDecay = 0.01;
// Below this, the storage check reports a warning instead of a tick.
constexpr qint64 kLowDiskBytes = 10LL * 1024 * 1024 * 1024;

QFrame* card(QWidget* parent = nullptr) {
    auto* f = new QFrame(parent);
    f->setObjectName(QStringLiteral("card"));
    return f;
}

QLabel* mono(const QString& s, const QString& tone = QStringLiteral("mute"), int px = 11) {
    return Theme::label(s, tone, px, false, true);
}

QLabel* text(const QString& s, const QString& tone = QString(), int px = 13, bool bold = false) {
    return Theme::label(s, tone, px, bold);
}

// Fixed-width label that ellipsises anything too long for its cell, keeping the
// full string as a tooltip. File and device names are user data and can be
// arbitrarily long; without this one long name stretches its column and skews
// the surrounding grid.
QLabel* elided(const QString& s, int width, const QString& tone = QString(),
               int px = 13, bool bold = false, bool useMono = false) {
    QLabel* l = Theme::label(QString(), tone, px, bold, useMono);
    l->setFixedWidth(width);
    // Elide against a slightly narrower budget: the label is measured with its
    // pre-polish font, and the stylesheet can substitute a marginally wider
    // family, which would clip the tail instead of ellipsising it.
    l->setText(l->fontMetrics().elidedText(s, Qt::ElideRight, qMax(16, width - 8)));
    l->setToolTip(s);
    return l;
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

QWidget* factPill(const QString& label, QLabel** valueOut) {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    auto* l = new QLabel(label.toUpper());
    l->setProperty("tone", "mute");
    QFont lf = l->font();
    lf.setPixelSize(10);
    lf.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
    l->setFont(lf);
    v->addWidget(l);
    auto* value = mono(QString(), QString(), 12);
    v->addWidget(value);
    if (valueOut) *valueOut = value;
    return w;
}

QString dbText(double v) {
    if (v < 0.001) return QStringLiteral("-inf dB");
    return QStringLiteral("%1 dB").arg(20.0 * std::log10(v), 0, 'f', 1);
}

// Drops every child of a re-rendered panel body. Widgets go through
// deleteLater() because a rebuild can be triggered from a signal one of them
// emitted; callers must clear any pointer they kept into the old content
// before calling this.
void clearLayout(QLayout* layout) {
    if (!layout) return;
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        if (QLayout* child = item->layout()) {
            clearLayout(child);
            child->deleteLater();
        }
        delete item;
    }
}

// One row of the system-status grid.
struct StatusCheck {
    QString icon;
    QString label;
    QString value;
    bool    ok = false;
};

} // namespace

Dashboard::Dashboard(SceneCollection* scenes,
                     AudioController* audio,
                     ClipsRegistry* clips,
                     ProjectRegistry* projects,
                     RenderQueue* renderQueue,
                     QWidget* parent)
    : QWidget(parent),
      m_scenes(scenes),
      m_audio(audio),
      m_clips(clips),
      m_projects(projects),
      m_renderQueue(renderQueue) {
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

    // Row 2: preview signal | recent recordings | render queue
    auto* r2 = new QHBoxLayout;
    r2->setSpacing(20);
    r2->addWidget(buildPreviewSignal(), 1);
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

    if (m_renderQueue)
        connect(m_renderQueue, &RenderQueue::changed, this, &Dashboard::refreshRenderQueue);
    if (m_clips)
        connect(m_clips, &ClipsRegistry::changed, this, &Dashboard::refreshClips);
    if (m_projects)
        connect(m_projects, &ProjectRegistry::changed, this, &Dashboard::refreshProjects);
    if (m_audio) {
        connect(m_audio, &AudioController::inputsChanged, this, &Dashboard::rebuildMeterRows);
        connect(m_audio, &AudioController::levelsUpdated, this, &Dashboard::onLevels);
        connect(m_audio, &AudioController::inputConnectionChanged, this, [this] {
            refreshOutputFacts();
            refreshSystemStatus();
        });
    }
    if (m_scenes) {
        connect(m_scenes, &SceneCollection::sourcesChanged, this, &Dashboard::refreshSceneSummary);
        connect(m_scenes, &SceneCollection::itemsChanged, this, &Dashboard::refreshSceneSummary);
        connect(m_scenes, &SceneCollection::sceneAdded, this, &Dashboard::refreshSceneSummary);
        connect(m_scenes, &SceneCollection::sceneRemoved, this, &Dashboard::refreshSceneSummary);
        connect(m_scenes, &SceneCollection::sceneRenamed, this, &Dashboard::refreshSceneSummary);
        connect(m_scenes, &SceneCollection::currentChanged, this, &Dashboard::refreshSceneSummary);
        connect(m_scenes, &SceneCollection::collectionReset, this, &Dashboard::refreshSceneSummary);
    }

    rebuildMeterRows();
    refreshAll();
}

void Dashboard::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // Output settings, the recording folder and the registries can all change
    // from other workspaces while the dashboard is stacked behind them.
    refreshAll();
}

void Dashboard::refreshAll() {
    refreshOutputFacts();
    refreshSceneSummary();
    refreshRecordings();
    refreshProjects();
    refreshClips();
    refreshRenderQueue();
    refreshSystemStatus();
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

// ---------------------------------------------------------------------------
// Hero
// ---------------------------------------------------------------------------

QWidget* Dashboard::buildHero() {
    auto* c = card();
    auto* v = new QVBoxLayout(c);
    v->setContentsMargins(24, 24, 24, 24);
    v->setSpacing(12);

    auto* top = new QHBoxLayout;
    top->setSpacing(8);
    top->addWidget(Theme::makeTag(tr("Project"), QStringLiteral("accent")));
    m_heroSubtitle = text(QString(), QStringLiteral("mute"), 12);
    top->addWidget(m_heroSubtitle);
    top->addStretch();
    v->addLayout(top);

    m_heroTitle = new QLabel(tr("Untitled project"));
    m_heroTitle->setObjectName(QStringLiteral("heroTitle"));
    v->addWidget(m_heroTitle);

    auto* desc = text(tr("Compose scenes, record and stream, then clip and edit, all in one workstation."),
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
    facts->addWidget(factPill(tr("Resolution"), &m_factResolution));
    facts->addWidget(factPill(tr("Framerate"), &m_factFramerate));
    facts->addWidget(factPill(tr("Audio"), &m_factAudio));
    facts->addWidget(factPill(tr("Mic"), &m_factMic));
    facts->addWidget(factPill(tr("Destination"), &m_factDestination));
    facts->addStretch();
    v->addLayout(facts);
    v->addStretch();

    return c;
}

void Dashboard::refreshOutputFacts() {
    const OutputSettings out = OutputSettings::load();

    if (m_factResolution)
        m_factResolution->setText(QStringLiteral("%1 x %2").arg(out.width).arg(out.height));
    if (m_factFramerate)
        m_factFramerate->setText(tr("%1 fps").arg(out.fps));
    if (m_previewMeta)
        m_previewMeta->setText(QStringLiteral("%1 x %2 · %3 fps")
                                   .arg(out.width).arg(out.height).arg(out.fps));

    if (m_factAudio) {
        if (m_audio) {
            const int channels = m_audio->channels();
            m_factAudio->setText(QStringLiteral("%1 kHz · %2")
                                     .arg(m_audio->sampleRate() / 1000)
                                     .arg(channels == 2 ? tr("Stereo") : tr("%1 ch").arg(channels)));
        } else {
            m_factAudio->setText(tr("No mix bus"));
        }
    }

    if (m_factMic) {
        QString mic = tr("None");
        if (m_audio) {
            for (const AudioInput& in : m_audio->inputs()) {
                if (in.loopback) continue;
                mic = in.connected ? in.name : tr("%1 (offline)").arg(in.name);
                break;
            }
        }
        m_factMic->setText(mic);
    }

    if (m_factDestination) {
        // The key itself is never rendered, only whether one is configured.
        const StreamSettings stream = StreamSettings::load();
        const QString service = StreamSettings::displayName(stream.service);
        m_factDestination->setText(stream.streamKey.isEmpty() ? tr("%1 · no key").arg(service)
                                                              : tr("%1 · key set").arg(service));
    }

    // Keep the replay quick action honest about the configured buffer length.
    if (m_clipTitle) {
        m_clipTitle->setText(out.replayBufferSeconds > 0
                                 ? tr("Clip last %1 s").arg(out.replayBufferSeconds)
                                 : tr("Clip last seconds"));
    }
    if (m_clipSub) {
        m_clipSub->setText(out.replayBufferSeconds > 0 ? tr("Replay buffer")
                                                       : tr("Replay buffer is off"));
    }
}

void Dashboard::refreshSceneSummary() {
    if (m_heroSubtitle) {
        if (m_scenes) {
            const int scenes = m_scenes->sceneCount();
            const int sources = m_scenes->sources().size();
            m_heroSubtitle->setText(tr("%n scene(s)", nullptr, scenes) + QStringLiteral(" · ")
                                    + tr("%n source(s)", nullptr, sources));
        } else {
            m_heroSubtitle->setText(QString());
        }
    }

    if (m_scenePlaceholder) {
        const Scene* current = m_scenes ? m_scenes->currentScene() : nullptr;
        m_scenePlaceholder->setLabel(current ? tr("Scene · %1").arg(current->name())
                                             : tr("No scene"));
    }
}

// ---------------------------------------------------------------------------
// Quick actions
// ---------------------------------------------------------------------------

QWidget* Dashboard::buildQuickActions() {
    auto* c = card();
    auto* v = new QVBoxLayout(c);
    v->setContentsMargins(16, 12, 16, 16);
    v->setSpacing(8);
    v->addWidget(Theme::makeSectionHeader(tr("Quick actions")));

    auto* grid = new QGridLayout;
    grid->setSpacing(8);

    // A QPushButton derives its size hint from its own (empty) text and ignores
    // a nested layout, so a grid cell squeezes the icon chip and the two text
    // lines until they overlap. Pin a content-sized height (same remedy as the
    // onboarding option cards, which share this style).
    auto addAction = [&](int index, const QString& icon, const QString& title,
                         const QString& sub, const QColor& color,
                         QLabel** titleOut, QLabel** subOut) {
        auto* b = new QPushButton;
        b->setObjectName(QStringLiteral("actionButton"));
        b->setCursor(Qt::PointingHandCursor);
        b->setMinimumHeight(52);
        auto* h = new QHBoxLayout(b);
        h->setContentsMargins(8, 8, 8, 8);
        h->setSpacing(12);
        h->addWidget(iconChip(icon, color, 32, 16));
        auto* tv = new QVBoxLayout;
        tv->setSpacing(0);
        auto* titleLabel = text(title, QString(), 13, true);
        auto* subLabel = text(sub, QStringLiteral("mute"), 11);
        tv->addWidget(titleLabel);
        tv->addWidget(subLabel);
        h->addLayout(tv);
        h->addStretch();
        grid->addWidget(b, index / 2, index % 2);
        if (titleOut) *titleOut = titleLabel;
        if (subOut) *subOut = subLabel;
        return b;
    };

    connect(addAction(0, QStringLiteral("record"), tr("Start Recording"),
                      tr("Current output profile"), Theme::RecHi, nullptr, nullptr),
            &QPushButton::clicked, this, &Dashboard::recordRequested);
    connect(addAction(1, QStringLiteral("stream"), tr("Go Live"),
                      tr("Streaming studio"), Theme::AccentHi, nullptr, nullptr),
            &QPushButton::clicked, this, &Dashboard::streamRequested);
    connect(addAction(2, QStringLiteral("editor"), tr("Open Editor"),
                      tr("Project timeline"), Theme::TextDim, nullptr, nullptr),
            &QPushButton::clicked, this, [this] { emit navigateTo(QStringLiteral("editor")); });
    connect(addAction(3, QStringLiteral("upload"), tr("Media Library"),
                      tr("Browse indexed folders"), Theme::TextDim, nullptr, nullptr),
            &QPushButton::clicked, this, [this] { emit navigateTo(QStringLiteral("media")); });
    connect(addAction(4, QStringLiteral("scissors"), tr("Clip last seconds"),
                      tr("Replay buffer"), Theme::TextDim, &m_clipTitle, &m_clipSub),
            &QPushButton::clicked, this, &Dashboard::saveReplayRequested);
    connect(addAction(5, QStringLiteral("projects"), tr("New Project"),
                      tr("From scratch"), Theme::TextDim, nullptr, nullptr),
            &QPushButton::clicked, this, &Dashboard::newProjectRequested);

    v->addLayout(grid);
    v->addStretch();
    return c;
}

// ---------------------------------------------------------------------------
// Preview signal: current scene plus the live mix bus
// ---------------------------------------------------------------------------

QWidget* Dashboard::buildPreviewSignal() {
    auto* panel = new PanelFrame(tr("Preview signal"), QStringLiteral("display"));
    m_previewMeta = mono(QString());
    panel->addHeaderWidget(m_previewMeta);

    auto* body = new QWidget;
    auto* v = new QVBoxLayout(body);
    v->setContentsMargins(12, 12, 12, 12);
    v->setSpacing(12);

    m_scenePlaceholder = new Placeholder(tr("No scene"), 16, 9);
    v->addWidget(m_scenePlaceholder);

    m_meterLayout = new QVBoxLayout;
    m_meterLayout->setSpacing(8);
    v->addLayout(m_meterLayout);
    v->addStretch();

    panel->bodyLayout()->addWidget(body);
    return panel;
}

void Dashboard::rebuildMeterRows() {
    if (!m_meterLayout) return;

    // Clear the lookup before the widgets go away: a level update can arrive
    // between the takeAt() below and the deferred deletion.
    m_meterRows.clear();
    clearLayout(m_meterLayout);

    const QList<AudioInput> inputs = m_audio ? m_audio->inputs() : QList<AudioInput>{};
    if (inputs.isEmpty()) {
        auto* empty = text(tr("No inputs on the mix bus."), QStringLiteral("mute"), 12);
        empty->setWordWrap(true);
        m_meterLayout->addWidget(empty);
        return;
    }

    for (const AudioInput& in : inputs) {
        auto* row = new QWidget;
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(8);

        const QString name = in.connected ? in.name : tr("%1 (offline)").arg(in.name);
        h->addWidget(elided(name, 110, QStringLiteral("dim"), 11));

        auto* meter = new MeterBar;
        h->addWidget(meter, 1);

        auto* db = mono(dbText(0.0), QStringLiteral("dim"));
        db->setFixedWidth(56);
        db->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        h->addWidget(db);

        m_meterLayout->addWidget(row);
        m_meterRows.insert(in.id, MeterRow{meter, db, 0.0, 0.0});
    }
}

void Dashboard::onLevels(const QString& id, float peakL, float peakR) {
    // The mix bus keeps running while another workspace is on screen; there is
    // no point repainting meters nobody can see.
    if (!isVisible()) return;

    auto it = m_meterRows.find(id);
    if (it == m_meterRows.end()) return;

    MeterRow& row = it.value();
    row.level = qMax(peakL, peakR);
    row.peak = row.level > row.peak ? row.level : qMax(row.level, row.peak - kPeakDecay);
    row.meter->setValues(row.level, row.peak);
    row.db->setText(dbText(row.peak));
}

// ---------------------------------------------------------------------------
// Recent recordings
// ---------------------------------------------------------------------------

QWidget* Dashboard::buildRecentRecordings() {
    auto* panel = new PanelFrame(tr("Recent recordings"), QStringLiteral("record"));
    auto* viewAll = new QPushButton(tr("View all"));
    Theme::setVariant(viewAll, QStringLiteral("ghost"));
    connect(viewAll, &QPushButton::clicked, this, [this] { emit navigateTo(QStringLiteral("media")); });
    panel->addHeaderWidget(viewAll);

    auto* body = new QWidget;
    m_recordingsLayout = new QVBoxLayout(body);
    m_recordingsLayout->setContentsMargins(6, 4, 6, 6);
    m_recordingsLayout->setSpacing(2);
    panel->bodyLayout()->addWidget(body);

    refreshRecordings();
    return panel;
}

void Dashboard::refreshRecordings() {
    if (!m_recordingsLayout) return;
    clearLayout(m_recordingsLayout);

    const QString dir = RecentRecordings::outputDir();
    const QVector<RecordingInfo> recordings = RecentRecordings::scan(dir, kRecentLimit);

    if (recordings.isEmpty()) {
        auto* empty = text(tr("No recordings in %1 yet.").arg(QDir::toNativeSeparators(dir)),
                           QStringLiteral("mute"), 12);
        empty->setWordWrap(true);
        m_recordingsLayout->addWidget(empty);
        m_recordingsLayout->addStretch();
        return;
    }

    for (const RecordingInfo& r : recordings) {
        auto* row = new QWidget;
        row->setToolTip(QDir::toNativeSeparators(r.filePath));
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(6, 4, 6, 4);
        h->setSpacing(8);
        h->addWidget(iconChip(QStringLiteral("record"), Theme::TextMute, 26, 12));
        auto* tv = new QVBoxLayout;
        tv->setSpacing(0);
        tv->addWidget(elided(r.name, 220, QString(), 12));
        tv->addWidget(mono(r.sizeText(), QStringLiteral("mute"), 10));
        h->addLayout(tv);
        h->addStretch();
        h->addWidget(text(r.relativeTimeText(), QStringLiteral("mute"), 11));
        m_recordingsLayout->addWidget(row);
    }
    m_recordingsLayout->addStretch();
}

// ---------------------------------------------------------------------------
// Render queue
// ---------------------------------------------------------------------------

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
    clearLayout(m_renderBodyLayout);

    using S = RenderJob::State;
    const QVector<RenderJob> all = m_renderQueue ? m_renderQueue->jobs() : QVector<RenderJob>{};
    if (m_renderMeta && m_renderQueue) {
        m_renderMeta->setText(tr("%1 active · %2 pending")
            .arg(m_renderQueue->countOfState(S::Active))
            .arg(m_renderQueue->countOfState(S::Pending)));
    }

    // Show active + pending jobs (up to 4), active first.
    int shown = 0;
    auto addJob = [&](const RenderJob& j, bool active) {
        auto* w = new QWidget;
        auto* jv = new QVBoxLayout(w);
        jv->setContentsMargins(0, 0, 0, 0);
        jv->setSpacing(4);
        auto* hr = new QHBoxLayout;
        hr->addWidget(elided(j.name, 150, QString(), 12));
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
    for (const RenderJob& j : all) { if (shown >= kRecentLimit) break; if (j.state == S::Pending) { addJob(j, false); ++shown; } }

    if (shown == 0) {
        auto* empty = text(tr("No renders queued. Start one from the Render workspace."),
                           QStringLiteral("mute"), 12);
        empty->setWordWrap(true);
        m_renderBodyLayout->addWidget(empty);
    }
    m_renderBodyLayout->addStretch();
}

// ---------------------------------------------------------------------------
// Recent projects
// ---------------------------------------------------------------------------

QWidget* Dashboard::buildRecentProjects() {
    auto* panel = new PanelFrame(tr("Recent projects"), QStringLiteral("projects"));
    auto* neu = new QPushButton(Icons::icon(QStringLiteral("plus"), Theme::Text, 12), tr(" New"));
    connect(neu, &QPushButton::clicked, this, &Dashboard::newProjectRequested);
    panel->addHeaderWidget(neu);

    auto* body = new QWidget;
    m_projectsLayout = new QGridLayout(body);
    m_projectsLayout->setContentsMargins(12, 12, 12, 12);
    m_projectsLayout->setSpacing(12);
    panel->bodyLayout()->addWidget(body);

    refreshProjects();
    return panel;
}

void Dashboard::refreshProjects() {
    if (!m_projectsLayout) return;
    clearLayout(m_projectsLayout);

    // Fixed column widths and a slack row below: with fewer than kRecentLimit
    // projects the cells would otherwise stretch and the covers balloon.
    for (int c = 0; c < kRecentLimit; ++c) m_projectsLayout->setColumnStretch(c, 1);
    m_projectsLayout->setRowStretch(1, 1);

    QVector<ProjectInfo> projects = m_projects ? m_projects->projects() : QVector<ProjectInfo>{};
    if (projects.isEmpty()) {
        auto* empty = text(tr("No projects found yet. Saved .malloy.json projects in your "
                              "Movies and Documents folders appear here."),
                           QStringLiteral("mute"), 12);
        empty->setWordWrap(true);
        m_projectsLayout->addWidget(empty, 0, 0, 1, kRecentLimit);
        return;
    }
    if (projects.size() > kRecentLimit) projects.resize(kRecentLimit);

    int i = 0;
    for (const ProjectInfo& p : projects) {
        auto* cell = new QWidget;
        auto* cv = new QVBoxLayout(cell);
        cv->setContentsMargins(0, 0, 0, 0);
        cv->setSpacing(6);
        cv->addWidget(new Placeholder(p.name, 16, 10));

        auto* titleRow = new QHBoxLayout;
        titleRow->setSpacing(6);
        titleRow->addWidget(elided(p.name, 130, QString(), 12, true));
        titleRow->addStretch();
        auto* open = new QPushButton(tr("Open"));
        Theme::setVariant(open, QStringLiteral("ghost"));
        open->setCursor(Qt::PointingHandCursor);
        const QString path = p.filePath;
        open->setToolTip(QDir::toNativeSeparators(path));
        connect(open, &QPushButton::clicked, this, [this, path] { emit openProjectRequested(path); });
        titleRow->addWidget(open);
        cv->addLayout(titleRow);

        const QString scenes = p.sceneCount >= 0 ? tr("%n scene(s)", nullptr, p.sceneCount)
                                                 : tr("MalloyStudio project");
        cv->addWidget(mono(QStringLiteral("%1 · %2 · %3")
                               .arg(scenes, p.sizeText(),
                                    p.modified.toString(QStringLiteral("MMM d"))),
                           QStringLiteral("mute"), 10));
        cv->addStretch();
        m_projectsLayout->addWidget(cell, 0, i++);
    }
}

// ---------------------------------------------------------------------------
// Recent clips
// ---------------------------------------------------------------------------

QWidget* Dashboard::buildRecentClips() {
    auto* panel = new PanelFrame(tr("Recent clips"), QStringLiteral("clips"));
    auto* lib = new QPushButton(tr("Clips library"));
    Theme::setVariant(lib, QStringLiteral("ghost"));
    connect(lib, &QPushButton::clicked, this, [this] { emit navigateTo(QStringLiteral("clips")); });
    panel->addHeaderWidget(lib);

    auto* body = new QWidget;
    m_clipsLayout = new QGridLayout(body);
    m_clipsLayout->setContentsMargins(12, 12, 12, 12);
    m_clipsLayout->setSpacing(12);
    panel->bodyLayout()->addWidget(body);

    refreshClips();
    return panel;
}

void Dashboard::refreshClips() {
    if (!m_clipsLayout) return;
    clearLayout(m_clipsLayout);

    for (int c = 0; c < 2; ++c) m_clipsLayout->setColumnStretch(c, 1);
    m_clipsLayout->setRowStretch(2, 1);

    QVector<ClipInfo> clips;
    if (m_clips) {
        for (const ClipInfo& c : m_clips->clips()) {
            if (c.archived) continue;
            clips.push_back(c);
            if (clips.size() >= kRecentLimit) break;
        }
    }

    if (clips.isEmpty()) {
        auto* empty = text(tr("No clips yet. Saving from the replay buffer adds them here."),
                           QStringLiteral("mute"), 12);
        empty->setWordWrap(true);
        m_clipsLayout->addWidget(empty, 0, 0, 1, 2);
        return;
    }

    int i = 0;
    for (const ClipInfo& c : clips) {
        auto* cell = new QWidget;
        cell->setToolTip(QDir::toNativeSeparators(c.filePath));
        auto* cv = new QVBoxLayout(cell);
        cv->setContentsMargins(0, 0, 0, 0);
        cv->setSpacing(6);
        cv->addWidget(new Placeholder(c.name, 16, 9));
        auto* meta = new QHBoxLayout;
        meta->setSpacing(6);
        if (!c.sourceProject.isEmpty()) meta->addWidget(Theme::makeTag(c.sourceProject));
        meta->addStretch();
        meta->addWidget(mono(c.durationText(), QStringLiteral("dim")));
        cv->addLayout(meta);
        cv->addStretch();
        m_clipsLayout->addWidget(cell, i / 2, i % 2);
        ++i;
    }
}

// ---------------------------------------------------------------------------
// System status
// ---------------------------------------------------------------------------

QWidget* Dashboard::buildSystemStatus() {
    auto* panel = new PanelFrame(tr("System status"), QStringLiteral("info"));
    m_statusMeta = text(QString(), QStringLiteral("mute"), 11);
    panel->addHeaderWidget(m_statusMeta);

    auto* body = new QWidget;
    m_statusLayout = new QGridLayout(body);
    m_statusLayout->setContentsMargins(12, 12, 12, 12);
    m_statusLayout->setSpacing(8);
    panel->bodyLayout()->addWidget(body);

    refreshSystemStatus();
    return panel;
}

void Dashboard::refreshSystemStatus() {
    if (!m_statusLayout) return;
    clearLayout(m_statusLayout);

    const OutputSettings out = OutputSettings::load();
    QVector<StatusCheck> checks;

    // Microphone: the first capture (non-loopback) input on the mix bus.
    {
        StatusCheck c{QStringLiteral("mic"), tr("Microphone"), tr("No input device"), false};
        if (m_audio) {
            for (const AudioInput& in : m_audio->inputs()) {
                if (in.loopback) continue;
                c.ok = in.connected;
                c.value = in.connected ? in.name : tr("%1 (disconnected)").arg(in.name);
                break;
            }
        }
        checks.push_back(c);
    }

    // Capture sources configured in the current collection. These report what
    // the project is set up to capture, not what hardware exists: enumerating
    // devices here would put a MediaFoundation probe (measured at ~3.5 s with
    // no camera attached) on the startup path, and the add-source dialogs
    // already validate the device when a source is created.
    {
        StatusCheck camera{QStringLiteral("camera"), tr("Camera"), tr("No camera source"), false};
        StatusCheck display{QStringLiteral("display"), tr("Display capture"), tr("No display source"), false};
        StatusCheck window{QStringLiteral("window"), tr("Window capture"), tr("No window source"), false};
        bool haveCamera = false;
        bool haveDisplay = false;
        bool haveWindow = false;

        if (m_scenes) {
            for (const Source* s : m_scenes->sources()) {
                switch (s->type()) {
                case Source::Type::Camera:
                    if (haveCamera) break;
                    haveCamera = true;
                    camera.ok = s->hasCameraConfig();
                    camera.value = camera.ok && !s->cameraName().isEmpty()
                                       ? s->cameraName()
                                       : tr("%1 (no device set)").arg(s->name());
                    break;
                case Source::Type::DisplayCapture:
                    if (haveDisplay) break;
                    haveDisplay = true;
                    display.ok = s->hasMonitorConfig();
                    display.value = display.ok
                                        ? tr("%1 · monitor %2").arg(s->name()).arg(s->outputIndex() + 1)
                                        : tr("%1 (no monitor set)").arg(s->name());
                    break;
                case Source::Type::WindowCapture:
                    if (haveWindow) break;
                    haveWindow = true;
                    window.ok = s->hasWindowConfig();
                    window.value = window.ok && !s->windowTitle().isEmpty()
                                       ? s->windowTitle()
                                       : tr("%1 (no window set)").arg(s->name());
                    break;
                default:
                    break;
                }
            }
        }
        checks.push_back(camera);
        checks.push_back(display);
        checks.push_back(window);
    }

    // Desktop audio: the loopback input on the mix bus.
    {
        StatusCheck c{QStringLiteral("speaker"), tr("Desktop audio"), tr("Loopback unavailable"), false};
        if (m_audio) {
            for (const AudioInput& in : m_audio->inputs()) {
                if (!in.loopback) continue;
                c.ok = in.connected;
                c.value = in.connected
                              ? tr("%1 · %2 kHz").arg(in.name).arg(m_audio->sampleRate() / 1000)
                              : tr("%1 (disconnected)").arg(in.name);
                break;
            }
        }
        checks.push_back(c);
    }

    // Encoder: whether the configured codec is one this machine can run.
    {
        const EncoderRegistry::Encoder* encoder = EncoderRegistry::find(out.videoCodec);
        checks.push_back({QStringLiteral("cpu"), tr("Encoder"),
                          encoder ? encoder->display : tr("%1 (unavailable)").arg(out.videoCodec),
                          encoder != nullptr});
    }

    // Storage: free space on the volume holding the recording folder.
    {
        const QString dir = RecentRecordings::outputDir();
        const QStorageInfo storage(dir);
        StatusCheck c{QStringLiteral("disk"), tr("Storage"), tr("Folder unavailable"), false};
        if (storage.isValid() && storage.isReady()) {
            const qint64 available = storage.bytesAvailable();
            c.value = tr("%1 free · %2").arg(formatByteSize(available),
                                             QDir::toNativeSeparators(storage.rootPath()));
            c.ok = available >= kLowDiskBytes;
        }
        checks.push_back(c);
    }

    // Stream key: presence only. The key is never displayed.
    {
        const StreamSettings stream = StreamSettings::load();
        const QString service = StreamSettings::displayName(stream.service);
        const bool configured = !stream.streamKey.isEmpty();
        checks.push_back({QStringLiteral("link"), tr("Stream key"),
                          configured ? tr("%1 · configured").arg(service)
                                     : tr("%1 · not configured").arg(service),
                          configured});
    }

    int ready = 0;
    int i = 0;
    for (const StatusCheck& s : checks) {
        if (s.ok) ++ready;
        auto* item = new QFrame;
        item->setObjectName(QStringLiteral("statusItem"));
        auto* h = new QHBoxLayout(item);
        h->setContentsMargins(12, 10, 12, 10);
        h->setSpacing(12);
        h->addWidget(iconChip(s.icon, s.ok ? Theme::TextDim : Theme::Warn, 28, 14));
        auto* tv = new QVBoxLayout;
        tv->setSpacing(0);
        auto* label = text(s.label.toUpper(), QStringLiteral("mute"), 11);
        QFont lf = label->font();
        lf.setLetterSpacing(QFont::AbsoluteSpacing, 0.5);
        label->setFont(lf);
        tv->addWidget(label);
        tv->addWidget(elided(s.value, 190, s.ok ? QString() : QStringLiteral("warn"), 12, false, true));
        h->addLayout(tv);
        h->addStretch();
        auto* mark = new QLabel;
        mark->setPixmap(Icons::pixmap(s.ok ? QStringLiteral("check") : QStringLiteral("alert"),
                                      s.ok ? Theme::Success : Theme::Warn, 14));
        h->addWidget(mark);
        m_statusLayout->addWidget(item, i / 4, i % 4);
        ++i;
    }

    if (m_statusMeta) {
        m_statusMeta->setText(ready == checks.size()
                                  ? tr("All systems ready")
                                  : tr("%1 of %2 ready").arg(ready).arg(checks.size()));
    }
}
