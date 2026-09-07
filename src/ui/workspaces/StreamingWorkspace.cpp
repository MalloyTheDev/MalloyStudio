#include "ui/workspaces/StreamingWorkspace.h"
#include "ui/components/PanelFrame.h"
#include "ui/components/Placeholder.h"
#include "ui/VuMeter.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"
#include "audio/AudioController.h"
#include "audio/AudioInput.h"
#include "recording/EncoderRegistry.h"
#include "recording/OutputSettings.h"

#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace {

QLabel* lbl(const QString& s, const QString& tone = QString(), int px = 13, bool bold = false, bool mono = false) {
    return Theme::label(s, tone, px, bold, mono);
}

// Shown wherever a figure is not known, so an unknown value is never confused
// with a measured one.
const QString kNoValue = QStringLiteral("-");

QWidget* metricTile(const QString& label, QLabel** valueOut, const QString& tone = QString()) {
    auto* f = new QFrame;
    f->setObjectName(QStringLiteral("statusItem"));
    auto* v = new QVBoxLayout(f);
    v->setContentsMargins(10, 8, 10, 8);
    v->setSpacing(2);
    auto* l = lbl(label.toUpper(), QStringLiteral("mute"), 10);
    QFont lf = l->font(); lf.setLetterSpacing(QFont::AbsoluteSpacing, 0.5); l->setFont(lf);
    v->addWidget(l);
    auto* val = lbl(kNoValue, tone, 13, true, true);
    v->addWidget(val);
    if (valueOut) *valueOut = val;
    return f;
}

} // namespace

StreamingWorkspace::StreamingWorkspace(AudioController* audio, QWidget* parent)
    : QWidget(parent), m_audio(audio)
{
    Q_ASSERT_X(m_audio, "StreamingWorkspace",
               "an AudioController is required so the Mix section can show real levels");
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    row->addWidget(buildCenter(), 1);
    row->addWidget(buildRail());

    // Mix wiring: same AudioController the Recording sidebar mixer observes.
    // Qt::AutoConnection delivers directly on the main thread, so changes made
    // here are immediately reflected in the Recording view (and vice versa).
    connect(m_audio, &AudioController::inputsChanged,
            this, &StreamingWorkspace::rebuildMixStrips);
    connect(m_audio, &AudioController::levelsUpdated,
            this, &StreamingWorkspace::onMixLevels);
    connect(m_audio, &AudioController::inputConnectionChanged,
            this, &StreamingWorkspace::onMixConnectionChanged);
    connect(m_audio, &AudioController::inputControlChanged,
            this, &StreamingWorkspace::onMixControlChanged);
    rebuildMixStrips();

    loadMeta();
    connect(m_titleEdit, &QLineEdit::editingFinished, this, &StreamingWorkspace::persistMeta);
    connect(m_catCombo->lineEdit(), &QLineEdit::editingFinished, this, &StreamingWorkspace::persistMeta);
    connect(m_catCombo, &QComboBox::activated, this, [this](int) { persistMeta(); });

    setLive(false);
}

void StreamingWorkspace::loadMeta() {
    m_settings = StreamSettings::load();
    // No stand-in title. This field is pushed to the channel on going live, so
    // anything put here that the user did not type would retitle a real
    // broadcast.
    m_titleEdit->setText(m_settings.title);
    if (!m_settings.category.isEmpty()) m_catCombo->setCurrentText(m_settings.category);
    m_destBtn->setText(QStringLiteral("%1  ▾").arg(StreamSettings::displayName(m_settings.service)));

    if (m_keyTag) {
        const bool hasKey = !m_settings.streamKey.isEmpty();
        m_keyTag->setText(hasKey ? tr("key set") : tr("no key"));
        Theme::setProp(m_keyTag, "tone", hasKey ? QStringLiteral("success")
                                                : QStringLiteral("warn"));
        Theme::repolish(m_keyTag);
    }

    const QStringList tags = m_settings.tags;
    if (auto* h = qobject_cast<QHBoxLayout*>(m_tagsHost->layout())) {
        while (QLayoutItem* it = h->takeAt(0)) {
            if (it->widget()) it->widget()->deleteLater();
            delete it;
        }
        for (const QString& t : tags) h->addWidget(Theme::makeTag(t));
        h->addStretch();
    }
}

void StreamingWorkspace::persistMeta() {
    // Reload first so we never clobber a stream key changed elsewhere.
    StreamSettings s = StreamSettings::load();
    s.title    = m_titleEdit->text();
    s.category = m_catCombo->currentText();
    s.save();
    m_settings = s;
}

QWidget* StreamingWorkspace::buildCenter() {
    auto* center = new QWidget(this);
    auto* v = new QVBoxLayout(center);
    v->setContentsMargins(16, 16, 16, 16);
    v->setSpacing(12);

    // Destination bar
    auto* dest = new QHBoxLayout;
    dest->setSpacing(8);
    dest->addWidget(Theme::makeTag(tr("Destination"), QStringLiteral("accent")));
    // Replaced by loadMeta() with the configured service.
    m_destBtn = new QPushButton(tr("Destination  ▾"));
    m_destBtn->setCursor(Qt::PointingHandCursor);
    dest->addWidget(m_destBtn);
    auto* add = new QPushButton(Icons::icon(QStringLiteral("plus"), Theme::TextDim, 12), tr(" Add destination"));
    Theme::setVariant(add, QStringLiteral("ghost"));
    dest->addWidget(add);
    dest->addStretch();
    m_liveTag = Theme::makeTag(tr("LIVE"), QStringLiteral("live"));
    dest->addWidget(m_liveTag);
    v->addLayout(dest);

    // Preview
    // The composited program output is not mirrored here yet; the recording
    // workspace holds the one live preview.
    v->addWidget(new Placeholder(tr("Program preview is in the Recording workspace"), 16, 9), 1);

    // Go-live card
    auto* go = new QFrame;
    go->setObjectName(QStringLiteral("card"));
    auto* gh = new QHBoxLayout(go);
    gh->setContentsMargins(14, 14, 14, 14);
    gh->setSpacing(12);

    auto* left = new QVBoxLayout;
    left->setSpacing(6);
    left->addWidget(lbl(tr("STREAM TITLE"), QStringLiteral("mute"), 11));
    m_titleEdit = new QLineEdit;
    m_titleEdit->setPlaceholderText(tr("Sent to the channel when you go live"));
    left->addWidget(m_titleEdit);
    auto* selers = new QHBoxLayout;
    selers->setSpacing(8);
    m_catCombo = new QComboBox;
    m_catCombo->setEditable(true);
    // Left empty on purpose. The category is sent to the channel, and there is
    // no endpoint wired up yet that can list the real ones, so the app offers
    // no names of its own.
    m_catCombo->lineEdit()->setPlaceholderText(tr("Category"));
    selers->addWidget(m_catCombo, 1);
    left->addLayout(selers);
    m_tagsHost = new QWidget;
    auto* tags = new QHBoxLayout(m_tagsHost);
    tags->setContentsMargins(0, 0, 0, 0);
    tags->setSpacing(4);
    left->addWidget(m_tagsHost);
    gh->addLayout(left, 1);

    auto* right = new QVBoxLayout;
    right->setSpacing(6);
    auto* badges = new QHBoxLayout;
    badges->addStretch();
    // Reflects whether a key is stored, which is all that is known before the
    // ingest accepts a connection. loadMeta() sets the text and tone.
    m_keyTag = Theme::makeTag(tr("no key"), QStringLiteral("warn"));
    badges->addWidget(m_keyTag);
    right->addLayout(badges);
    right->addStretch();
    m_goLive = new QPushButton(tr("Go Live · F8"));
    Theme::setVariant(m_goLive, QStringLiteral("rec"));
    m_goLive->setMinimumWidth(200);
    m_goLive->setCursor(Qt::PointingHandCursor);
    connect(m_goLive, &QPushButton::clicked, this, &StreamingWorkspace::goLiveRequested);
    right->addWidget(m_goLive, 0, Qt::AlignRight);
    gh->addLayout(right);

    v->addWidget(go);
    return center;
}

QWidget* StreamingWorkspace::buildRail() {
    auto* rail = new QWidget(this);
    rail->setObjectName(QStringLiteral("recSideCol"));
    rail->setFixedWidth(380);
    auto* v = new QVBoxLayout(rail);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    // Stream health
    auto* health = new PanelFrame(tr("Stream health"), QStringLiteral("info"));
    m_statusTag = Theme::makeTag(tr("OFFLINE"));
    health->addHeaderWidget(m_statusTag);
    auto* hbody = new QWidget;
    auto* hgrid = new QGridLayout(hbody);
    hgrid->setContentsMargins(10, 10, 10, 10);
    hgrid->setSpacing(8);
    // Bitrate and dropped frames are measured; onStreamProgress() fills them.
    hgrid->addWidget(metricTile(tr("Bitrate"), &m_mBitrate), 0, 0);
    hgrid->addWidget(metricTile(tr("Dropped"), &m_mDropped), 0, 1);
    // The configured encoder, resolved to the name the settings page shows.
    hgrid->addWidget(metricTile(tr("Encoder"), &m_mEncoder), 1, 0);
    // Kept visible because it is the figure people look for, and left blank
    // because nothing here knows it. Filling it in needs the channel queried.
    auto* viewersTile = metricTile(tr("Viewers"), &m_mViewers);
    viewersTile->setToolTip(tr("Viewer count needs a connected Twitch account. "
                               "It is not available yet."));
    hgrid->addWidget(viewersTile, 1, 1);
    health->bodyLayout()->addWidget(hbody);
    v->addWidget(health);

    // Chat
    auto* chat = new PanelFrame(tr("Chat"), QStringLiteral("browser"));
    auto* chatBody = new QWidget;
    auto* cv = new QVBoxLayout(chatBody);
    cv->setContentsMargins(0, 0, 0, 0);
    cv->setSpacing(0);

    m_chatEmpty = new QWidget;
    auto* ce = new QVBoxLayout(m_chatEmpty);
    ce->setAlignment(Qt::AlignCenter);
    ce->setSpacing(6);
    auto* cicon = new QLabel; cicon->setPixmap(Icons::pixmap(QStringLiteral("browser"), Theme::TextFaint, 32));
    cicon->setAlignment(Qt::AlignCenter);
    ce->addWidget(cicon);
    auto* chatHead = lbl(tr("Chat is not connected yet."), QStringLiteral("dim"), 12);
    chatHead->setAlignment(Qt::AlignHCenter);
    ce->addWidget(chatHead);
    // Wrapped, because the rail is narrower than this sentence.
    auto* chatWhy = lbl(tr("Reading chat needs a connection this build does not have."),
                        QStringLiteral("mute"), 11);
    chatWhy->setWordWrap(true);
    chatWhy->setAlignment(Qt::AlignHCenter);
    ce->addWidget(chatWhy);
    cv->addWidget(m_chatEmpty, 1);

    auto* inputWrap = new QWidget;
    auto* iw = new QHBoxLayout(inputWrap);
    iw->setContentsMargins(8, 8, 8, 8);
    m_chatInput = new QLineEdit;
    // Disabled rather than merely empty: there is nowhere for a message to go,
    // and a box that accepts text it silently discards is worse than one that
    // refuses it.
    m_chatInput->setEnabled(false);
    m_chatInput->setPlaceholderText(tr("Chat is not connected yet"));
    iw->addWidget(m_chatInput);
    cv->addWidget(inputWrap);

    chat->bodyLayout()->addWidget(chatBody);
    v->addWidget(chat, 1);

    // Alerts
    auto* alerts = new PanelFrame(tr("Alerts"), QStringLiteral("bell"));
    auto* mute = new QPushButton(tr("Mute"));
    Theme::setVariant(mute, QStringLiteral("ghost"));
    alerts->addHeaderWidget(mute);
    auto* abody = new QWidget;
    auto* av = new QVBoxLayout(abody);
    av->setContentsMargins(10, 10, 10, 10);
    av->setSpacing(4);
    m_alertsEmpty = lbl(tr("Alerts are not connected yet. Subscriptions, follows and bits "
                           "need a channel event connection this build does not have."),
                        QStringLiteral("mute"), 12);
    m_alertsEmpty->setWordWrap(true);
    av->addWidget(m_alertsEmpty);
    alerts->bodyLayout()->addWidget(abody);
    v->addWidget(alerts);

    // Mix (real, AudioController-driven). One channel strip per input — the
    // strip set is populated/refreshed by rebuildMixStrips() in response to
    // AudioController::inputsChanged. An empty-state hint covers the
    // (defensive) case where loopback:default fails to add.
    auto* mix = new PanelFrame(tr("Mix"), QStringLiteral("speaker"));
    auto* mbody = new QWidget;
    auto* mv = new QVBoxLayout(mbody);
    mv->setContentsMargins(10, 10, 10, 10);
    mv->setSpacing(8);
    m_mixLanes = new QVBoxLayout;
    m_mixLanes->setContentsMargins(0, 0, 0, 0);
    m_mixLanes->setSpacing(6);
    mv->addLayout(m_mixLanes);
    m_mixEmpty = lbl(tr("No audio inputs detected."), QStringLiteral("mute"), 11);
    m_mixEmpty->setAlignment(Qt::AlignCenter);
    m_mixEmpty->setVisible(false);
    mv->addWidget(m_mixEmpty);
    mix->bodyLayout()->addWidget(mbody);
    v->addWidget(mix);

    return rail;
}

namespace {
// The label the settings page would show for a codec id, so the health panel
// names the encoder the same way the rest of the app does.
QString encoderDisplayName(const QString& codecId) {
    for (const EncoderRegistry::Encoder& e : EncoderRegistry::available())
        if (e.id == codecId) return e.display;
    return codecId;
}
}  // namespace

void StreamingWorkspace::setLive(bool live) {
    m_live = live;
    m_dropped = 0;

    m_liveTag->setVisible(live);

    // Chat and alerts have no source, live or not, so their empty states stay
    // put. Going live used to swap in canned messages, which made the app least
    // truthful at the moment it was most likely to be believed.
    m_chatEmpty->setVisible(true);
    m_alertsEmpty->setVisible(true);

    // Nothing is known about the connection until the first progress line
    // arrives, so this reports the state of the app, not of the stream.
    Theme::setProp(m_statusTag, "tone", QString());
    m_statusTag->setText(live ? tr("LIVE") : tr("OFFLINE"));
    Theme::repolish(m_statusTag);

    m_goLive->setText(live ? tr("End stream") : tr("Go Live · F8"));
    Theme::setVariant(m_goLive, live ? QStringLiteral("outlineRec") : QStringLiteral("rec"));

    if (m_mEncoder)
        m_mEncoder->setText(live ? encoderDisplayName(OutputSettings::load().videoCodec)
                                 : kNoValue);

    if (!live) {
        for (QLabel* m : {m_mViewers, m_mBitrate, m_mDropped})
            if (m) m->setText(kNoValue);
        if (m_mDropped) Theme::setTone(m_mDropped, QString());
    }
}

void StreamingWorkspace::onStreamProgress(int bitrateKbps, int droppedFrames) {
    if (!m_live) return;

    // ffmpeg omits the drop token on some lines, which parses as zero. Taking
    // the maximum keeps the counter from walking backwards mid-stream.
    m_dropped = std::max(m_dropped, droppedFrames);

    if (m_mBitrate) {
        m_mBitrate->setText(bitrateKbps > 0
            ? tr("%1 Mb/s").arg(bitrateKbps / 1000.0, 0, 'f', 1)
            : kNoValue);
    }
    if (m_mDropped) {
        m_mDropped->setText(QString::number(m_dropped));
        Theme::setTone(m_mDropped, m_dropped > 0 ? QStringLiteral("warn") : QString());
    }

    // The one health verdict there is evidence for. Dropped frames mean the
    // encoder or the connection is not keeping up; nothing else here is known.
    if (m_statusTag) {
        const bool dropping = m_dropped > 0;
        Theme::setProp(m_statusTag, "tone",
                       dropping ? QStringLiteral("warn") : QStringLiteral("success"));
        m_statusTag->setText(dropping ? tr("DROPPING") : tr("SENDING"));
        Theme::repolish(m_statusTag);
    }
}

// ── Mix: AudioController-driven channel strips ─────────────────────────────

StreamingWorkspace::Strip StreamingWorkspace::makeMixStrip(const QString& id, const AudioInput& in) {
    // Two-row compact strip:
    //   top:    [name (stretch)]  [M mute]
    //   bottom: [VuMeter]         [volume slider]
    // No pan — the streaming bus rarely needs per-input pan, and the rail is
    // narrower than the Recording sidebar. Pan can still be adjusted from the
    // Recording mixer if needed (the controller is shared).
    Strip s;
    s.root = new QWidget(this);
    auto* col = new QVBoxLayout(s.root);
    col->setContentsMargins(6, 4, 6, 4);
    col->setSpacing(4);

    auto* topRow = new QHBoxLayout;
    topRow->setSpacing(8);
    s.name = new QLabel(in.name, s.root);
    QFont f = s.name->font(); f.setBold(true); s.name->setFont(f);
    topRow->addWidget(s.name, 1);

    s.mute = new QToolButton(s.root);
    s.mute->setText(tr("M"));
    s.mute->setCheckable(true);
    s.mute->setChecked(in.muted);
    s.mute->setToolTip(tr("Mute"));
    s.mute->setFixedWidth(28);
    topRow->addWidget(s.mute);
    col->addLayout(topRow);

    auto* botRow = new QHBoxLayout;
    botRow->setSpacing(8);
    s.meter = new VuMeter(s.root);
    s.meter->setMinimumWidth(40);
    botRow->addWidget(s.meter, 2);

    s.volume = new QSlider(Qt::Horizontal, s.root);
    s.volume->setRange(0, 150);   // matches AudioMixerPanel: 0..150 → 0.0..1.5
    s.volume->setValue(static_cast<int>(in.volume * 100.0f));
    s.volume->setMinimumWidth(56);
    s.volume->setToolTip(tr("Volume (0–150 %)"));
    botRow->addWidget(s.volume, 2);
    col->addLayout(botRow);

    // Connectivity styling (matches the Recording mixer's pattern).
    s.meter->setEnabled(in.connected);
    s.volume->setEnabled(in.connected);
    s.mute->setEnabled(in.connected);
    if (!in.connected)
        s.name->setStyleSheet(QStringLiteral("color: #70737a; font-style: italic;"));

    // Push UI changes back to the controller. Capture id by value so the
    // lambda remains valid even after rebuildMixStrips() destroys the strip
    // and reuses the same id later.
    connect(s.volume, &QSlider::valueChanged, this, [this, id](int v) {
        m_audio->setVolume(id, float(v) / 100.0f);
    });
    connect(s.mute, &QToolButton::toggled, this, [this, id](bool m) {
        m_audio->setMuted(id, m);
    });

    return s;
}

void StreamingWorkspace::rebuildMixStrips() {
    if (!m_mixLanes) return;
    // Wipe existing strips wholesale — re-seeding via setValue would re-fire
    // the volume/mute lambdas, and recreating is also the simplest way to
    // handle an input being removed.
    for (auto it = m_mixStrips.begin(); it != m_mixStrips.end(); ++it)
        if (it.value().root) it.value().root->deleteLater();
    m_mixStrips.clear();

    const QList<AudioInput>& inputs = m_audio->inputs();
    for (const AudioInput& in : inputs) {
        Strip s = makeMixStrip(in.id, in);
        m_mixLanes->addWidget(s.root);
        m_mixStrips.insert(in.id, s);
    }
    if (m_mixEmpty) m_mixEmpty->setVisible(inputs.isEmpty());
}

void StreamingWorkspace::onMixLevels(const QString& id, float peakL, float peakR) {
    auto it = m_mixStrips.find(id);
    if (it == m_mixStrips.end()) return;
    it.value().meter->setLevels(peakL, peakR);
}

void StreamingWorkspace::onMixConnectionChanged(const QString& id, bool connected) {
    auto it = m_mixStrips.find(id);
    if (it == m_mixStrips.end()) return;
    it.value().meter->setEnabled(connected);
    it.value().volume->setEnabled(connected);
    it.value().mute->setEnabled(connected);
    it.value().name->setStyleSheet(connected
        ? QString()
        : QStringLiteral("color: #70737a; font-style: italic;"));
}

void StreamingWorkspace::onMixControlChanged(const QString& id) {
    // Mirror onInputControlChanged in AudioMixerPanel: re-seed our slider/mute
    // from the controller when the other view changed them. QSignalBlocker
    // prevents this re-seed from firing the controller's setters again.
    auto it = m_mixStrips.find(id);
    if (it == m_mixStrips.end()) return;
    for (const AudioInput& in : m_audio->inputs()) {
        if (in.id != id) continue;
        const Strip& s = it.value();
        const QSignalBlocker bv(s.volume), bm(s.mute);
        s.volume->setValue(static_cast<int>(in.volume * 100.0f));
        s.mute->setChecked(in.muted);
        return;
    }
}
