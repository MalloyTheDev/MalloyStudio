#include "ui/workspaces/StreamingWorkspace.h"
#include "ui/components/MeterBar.h"
#include "ui/components/PanelFrame.h"
#include "ui/components/Placeholder.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"

#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QTimer>
#include <QVBoxLayout>

namespace {

QLabel* lbl(const QString& s, const QString& tone = QString(), int px = 13, bool bold = false, bool mono = false) {
    return Theme::label(s, tone, px, bold, mono);
}

QWidget* metricTile(const QString& label, QLabel** valueOut, const QString& tone = QString()) {
    auto* f = new QFrame;
    f->setObjectName(QStringLiteral("statusItem"));
    auto* v = new QVBoxLayout(f);
    v->setContentsMargins(10, 8, 10, 8);
    v->setSpacing(2);
    auto* l = lbl(label.toUpper(), QStringLiteral("mute"), 10);
    QFont lf = l->font(); lf.setLetterSpacing(QFont::AbsoluteSpacing, 0.5); l->setFont(lf);
    v->addWidget(l);
    auto* val = lbl(QStringLiteral("—"), tone, 13, true, true);
    v->addWidget(val);
    if (valueOut) *valueOut = val;
    return f;
}

} // namespace

StreamingWorkspace::StreamingWorkspace(QWidget* parent) : QWidget(parent) {
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    row->addWidget(buildCenter(), 1);
    row->addWidget(buildRail());

    loadMeta();
    connect(m_titleEdit, &QLineEdit::editingFinished, this, &StreamingWorkspace::persistMeta);
    connect(m_catCombo->lineEdit(), &QLineEdit::editingFinished, this, &StreamingWorkspace::persistMeta);
    connect(m_catCombo, &QComboBox::activated, this, [this](int) { persistMeta(); });

    m_timer = new QTimer(this);
    m_timer->setInterval(100);
    connect(m_timer, &QTimer::timeout, this, &StreamingWorkspace::tick);
    m_timer->start();

    setLive(false);
}

namespace {
QStringList defaultStreamTags() {
    return {QObject::tr("Souls-like"), QObject::tr("No-hit"),
            QObject::tr("Phase 3"), QObject::tr("Late-night")};
}
}

void StreamingWorkspace::loadMeta() {
    m_settings = StreamSettings::load();
    m_titleEdit->setText(m_settings.title.isEmpty()
        ? tr("Spire — No-hit attempt night 4 · Phase 3 grind") : m_settings.title);
    if (!m_settings.category.isEmpty()) m_catCombo->setCurrentText(m_settings.category);
    m_destBtn->setText(QStringLiteral("%1  ▾").arg(StreamSettings::displayName(m_settings.service)));

    QStringList tags = m_settings.tags.isEmpty() ? defaultStreamTags() : m_settings.tags;
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
    if (s.tags.isEmpty()) s.tags = defaultStreamTags();
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
    m_destBtn = new QPushButton(tr("Twitch · /malloy_live  ▾"));
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
    v->addWidget(new Placeholder(tr("Program · Gameplay scene"), 16, 9), 1);

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
    left->addWidget(m_titleEdit);
    auto* selers = new QHBoxLayout;
    selers->setSpacing(8);
    m_catCombo = new QComboBox;
    m_catCombo->setEditable(true);
    m_catCombo->addItems({tr("Spire of the Hollow Sun"), tr("Just Chatting"),
                          tr("Software & Game Dev"), tr("Retro")});
    auto* lang = new QComboBox; lang->addItem(tr("Language · English")); lang->setFixedWidth(180);
    selers->addWidget(m_catCombo, 1);
    selers->addWidget(lang);
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
    badges->addWidget(Theme::makeTag(tr("REC + STREAM")));
    badges->addWidget(Theme::makeTag(tr("key verified"), QStringLiteral("success")));
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
    hgrid->addWidget(metricTile(tr("Viewers"), &m_mViewers, QStringLiteral("accent")), 0, 0);
    hgrid->addWidget(metricTile(tr("Bitrate"), &m_mBitrate), 0, 1);
    hgrid->addWidget(metricTile(tr("Dropped"), &m_mDropped), 1, 0);
    hgrid->addWidget(metricTile(tr("Ping"), &m_mPing), 1, 1);
    hgrid->addWidget(metricTile(tr("FPS"), &m_mFps), 2, 0);
    { QLabel* enc = nullptr; auto* t = metricTile(tr("Encoder"), &enc); enc->setText(QStringLiteral("NVENC · H.264")); hgrid->addWidget(t, 2, 1); }
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
    ce->addWidget(lbl(tr("Chat appears when you go live."), QStringLiteral("dim"), 12), 0, Qt::AlignHCenter);
    ce->addWidget(lbl(tr("Twitch · /malloy_live"), QStringLiteral("mute"), 11), 0, Qt::AlignHCenter);
    cv->addWidget(m_chatEmpty, 1);

    m_chatList = new QWidget;
    auto* cl = new QVBoxLayout(m_chatList);
    cl->setContentsMargins(10, 6, 10, 6);
    cl->setSpacing(4);
    struct M { QString u, m, tone; };
    const QVector<M> msgs = {
        {QStringLiteral("rekka_dev"), tr("GG that was insane"), QStringLiteral("accent")},
        {QStringLiteral("gorm"), tr("how did he dodge that????"), QString()},
        {QStringLiteral("phaseseven"), tr("phase 3 nightmare run"), QString()},
        {QStringLiteral("lunalux"), tr("first time here, this slaps"), QString()},
        {QStringLiteral("malloy_bot"), tr("phaseseven just subscribed (3 mo)"), QStringLiteral("warn")},
        {QStringLiteral("kx"), tr("audio is so clean"), QString()},
    };
    for (const M& m : msgs) {
        auto* line = new QLabel(QStringLiteral("<b style='color:%1'>%2</b> <span style='color:%3'>%4</span>")
            .arg((m.tone == "accent" ? Theme::AccentHi : m.tone == "warn" ? Theme::Warn : Theme::TextDim).name(),
                 m.u, (m.tone == "warn" ? Theme::Warn : Theme::Text).name(), m.m));
        line->setWordWrap(true);
        QFont lf = line->font(); lf.setPixelSize(12); line->setFont(lf);
        cl->addWidget(line);
    }
    cl->addStretch();
    cv->addWidget(m_chatList, 1);

    auto* inputWrap = new QWidget;
    auto* iw = new QHBoxLayout(inputWrap);
    iw->setContentsMargins(8, 8, 8, 8);
    m_chatInput = new QLineEdit;
    m_chatInput->setPlaceholderText(tr("Chat (offline)"));
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
    m_alertsEmpty = lbl(tr("No alerts. Go live to start receiving them."), QStringLiteral("mute"), 12);
    av->addWidget(m_alertsEmpty);
    m_alertsList = new QWidget;
    auto* al = new QVBoxLayout(m_alertsList);
    al->setContentsMargins(0, 0, 0, 0);
    al->setSpacing(4);
    for (const QString& a : {tr("phaseseven · Tier 1 · 3-month resub"), tr("kx_dev followed"), tr("wrenly cheered 500 bits")}) {
        auto* r = lbl(a, QStringLiteral("dim"), 12);
        al->addWidget(r);
    }
    av->addWidget(m_alertsList);
    alerts->bodyLayout()->addWidget(abody);
    v->addWidget(alerts);

    // Mix
    auto* mix = new PanelFrame(tr("Mix"), QStringLiteral("speaker"));
    auto* mbody = new QWidget;
    auto* mv = new QVBoxLayout(mbody);
    mv->setContentsMargins(10, 10, 10, 10);
    mv->setSpacing(8);
    auto addMeter = [&](const QString& name, MeterBar** out) {
        auto* h = new QHBoxLayout; h->setSpacing(8);
        auto* l = lbl(name, QStringLiteral("dim"), 11); l->setFixedWidth(60);
        h->addWidget(l);
        auto* m = new MeterBar; h->addWidget(m, 1);
        mv->addLayout(h);
        if (out) *out = m;
    };
    addMeter(tr("Mic"), &m_micMeter);
    addMeter(tr("Desktop"), &m_deskMeter);
    mix->bodyLayout()->addWidget(mbody);
    v->addWidget(mix);

    return rail;
}

void StreamingWorkspace::setLive(bool live) {
    m_live = live;
    m_elapsed = 0;
    if (!live) { m_viewers = 0; m_dropped = 0; }
    else if (m_viewers == 0) m_viewers = 1240;

    m_liveTag->setVisible(live);
    m_chatEmpty->setVisible(!live);
    m_chatList->setVisible(live);
    m_alertsEmpty->setVisible(!live);
    m_alertsList->setVisible(live);
    m_chatInput->setEnabled(live);
    m_chatInput->setPlaceholderText(live ? tr("Say something…") : tr("Chat (offline)"));

    Theme::setProp(m_statusTag, "tone", live ? QStringLiteral("success") : QString());
    m_statusTag->setText(live ? tr("GOOD") : tr("OFFLINE"));
    Theme::repolish(m_statusTag);

    m_goLive->setText(live ? tr("End stream") : tr("Go Live · F8"));
    Theme::setVariant(m_goLive, live ? QStringLiteral("outlineRec") : QStringLiteral("rec"));

    if (!live) {
        for (QLabel* m : {m_mViewers, m_mBitrate, m_mDropped, m_mPing, m_mFps})
            if (m) m->setText(QStringLiteral("—"));
    }
}

void StreamingWorkspace::tick() {
    auto* rng = QRandomGenerator::global();
    auto step = [&](double& level, double& peak, double base, double variance) {
        const double target = qBound(0.05, base + (rng->generateDouble() - 0.5) * variance * 2, 0.95);
        level += (target - level) * 0.35;
        peak = (level > peak) ? level : qMax(level, peak - 0.01);
    };
    step(m_micL, m_micP, 0.45, 0.22);
    step(m_deskL, m_deskP, 0.55, 0.18);
    if (m_micMeter) m_micMeter->setValues(m_micL, m_micP);
    if (m_deskMeter) m_deskMeter->setValues(m_deskL, m_deskP);

    if (m_live) {
        m_viewers = qBound(800, m_viewers + int((rng->generateDouble() - 0.35) * 80), 8000);
        if (rng->generateDouble() > 0.97) m_dropped += int(rng->generateDouble() * 3);
        m_mViewers->setText(QString::number(m_viewers));
        m_mBitrate->setText(QStringLiteral("%1 Mb/s").arg((5800 + rng->generateDouble() * 1200) / 1000.0, 0, 'f', 1));
        m_mDropped->setText(QString::number(m_dropped));
        m_mPing->setText(QStringLiteral("%1 ms").arg(22 + int(rng->generateDouble() * 18)));
        m_mFps->setText(QStringLiteral("59.8"));
    }
}
