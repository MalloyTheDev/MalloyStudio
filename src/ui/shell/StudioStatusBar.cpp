#include "ui/shell/StudioStatusBar.h"
#include "ui/Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QRandomGenerator>
#include <QTimer>

namespace {
QLabel* dot(const QColor& c, QWidget* parent) {
    auto* d = new QLabel(parent);
    d->setFixedSize(8, 8);
    d->setStyleSheet(QStringLiteral("background:%1;border-radius:4px;").arg(c.name()));
    return d;
}
}

StudioStatusBar::StudioStatusBar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("statusBar"));
    setFixedHeight(28);

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(16, 0, 16, 0);
    row->setSpacing(16);

    // State chip (dot + text) and the transient message that replaces it.
    m_stateChip = new QWidget(this);
    auto* sc = new QHBoxLayout(m_stateChip);
    sc->setContentsMargins(0, 0, 0, 0);
    sc->setSpacing(6);
    m_stateDot = dot(Theme::Success, m_stateChip);
    m_stateText = new QLabel(m_stateChip);
    m_stateText->setProperty("mono", true);
    sc->addWidget(m_stateDot);
    sc->addWidget(m_stateText);
    row->addWidget(m_stateChip);

    m_message = new QLabel(this);
    m_message->setProperty("tone", "dim");
    m_message->setVisible(false);
    row->addWidget(m_message);

    // Telemetry
    row->addWidget(makeStat(QStringLiteral("CPU"),  &m_cpu));
    row->addWidget(makeStat(QStringLiteral("RAM"),  &m_ram));
    row->addWidget(makeStat(QStringLiteral("DISK"), &m_disk));
    {
        QLabel* gpu = nullptr;
        auto* w = makeStat(QStringLiteral("GPU"), &gpu);
        gpu->setText(QStringLiteral("NVENC · 18%"));
        row->addWidget(w);
    }
    row->addWidget(makeStat(QStringLiteral("FPS"), &m_fps));
    m_bitrateStat = makeStat(QStringLiteral("BITRATE"), &m_bitrate);
    m_bitrateStat->setVisible(false);
    row->addWidget(m_bitrateStat);

    row->addStretch();

    auto* ver = new QLabel(QStringLiteral("v8.0.0  ·  ffmpeg 7.0.2"), this);
    ver->setProperty("mono", true);
    ver->setProperty("tone", "mute");
    row->addWidget(ver);

    m_statsTimer = new QTimer(this);
    m_statsTimer->setInterval(1000);
    connect(m_statsTimer, &QTimer::timeout, this, &StudioStatusBar::tickStats);
    m_statsTimer->start();

    m_clock = new QTimer(this);
    m_clock->setInterval(1000);
    connect(m_clock, &QTimer::timeout, this, &StudioStatusBar::tickClock);

    m_pulse = new QTimer(this);
    m_pulse->setInterval(700);
    connect(m_pulse, &QTimer::timeout, this, [this] {
        m_dotBright = !m_dotBright;
        const QColor c = (m_mode == Mode::Recording || m_mode == Mode::Streaming)
                             ? Theme::Rec : Theme::Success;
        const QColor shown = m_dotBright ? c : c.darker(160);
        m_stateDot->setStyleSheet(QStringLiteral("background:%1;border-radius:4px;").arg(shown.name()));
    });

    m_messageTimer = new QTimer(this);
    m_messageTimer->setSingleShot(true);
    connect(m_messageTimer, &QTimer::timeout, this, [this] {
        m_message->clear();
        m_message->setVisible(false);
        m_stateChip->setVisible(true);
    });

    tickStats();
    refreshState();
}

QWidget* StudioStatusBar::makeStat(const QString& label, QLabel** valueOut) {
    auto* w = new QWidget(this);
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(6);
    auto* l = new QLabel(label, w);
    l->setObjectName(QStringLiteral("statLabel"));
    auto* v = new QLabel(QStringLiteral("—"), w);
    v->setObjectName(QStringLiteral("statValue"));
    h->addWidget(l);
    h->addWidget(v);
    if (valueOut) *valueOut = v;
    return w;
}

void StudioStatusBar::setMode(Mode mode) {
    if (m_mode == mode) return;
    m_mode = mode;
    m_elapsed = 0;
    const bool active = (mode == Mode::Recording || mode == Mode::Streaming);
    if (active) m_clock->start(); else m_clock->stop();
    if (active) m_pulse->start();
    else { m_pulse->stop(); }
    m_bitrateStat->setVisible(active);
    refreshState();
}

void StudioStatusBar::refreshState() {
    QColor c = Theme::Success;
    QString text = QStringLiteral("IDLE");
    QString tone = QStringLiteral("success");
    auto hms = [](int s) {
        return QStringLiteral("%1:%2:%3")
            .arg(s / 3600, 2, 10, QChar('0'))
            .arg((s % 3600) / 60, 2, 10, QChar('0'))
            .arg(s % 60, 2, 10, QChar('0'));
    };
    switch (m_mode) {
    case Mode::Recording: c = Theme::Rec; text = QStringLiteral("REC %1").arg(hms(m_elapsed)); tone = "rec"; break;
    case Mode::Streaming: c = Theme::Rec; text = QStringLiteral("LIVE %1").arg(hms(m_elapsed)); tone = "rec"; break;
    case Mode::Rendering: c = Theme::AccentHi; text = QStringLiteral("RENDERING"); tone = "accent"; break;
    case Mode::Idle:      break;
    }
    m_stateDot->setStyleSheet(QStringLiteral("background:%1;border-radius:4px;").arg(c.name()));
    m_stateText->setText(text);
    Theme::setTone(m_stateText, tone);
}

void StudioStatusBar::tickClock() {
    ++m_elapsed;
    refreshState();
}

void StudioStatusBar::tickStats() {
    auto* rng = QRandomGenerator::global();
    const bool busy = (m_mode != Mode::Idle);
    m_vCpu  = qBound(5.0,  m_vCpu  + (rng->generateDouble() - 0.5) * (busy ? 14 : 4), 95.0);
    m_vRam  = qBound(20.0, m_vRam  + (rng->generateDouble() - 0.5) * 2, 85.0);
    m_vDisk = qBound(40.0, m_vDisk + (rng->generateDouble() - 0.5) * 0.4, 95.0);
    m_vFps  = qBound(55.0, 60.0 - rng->generateDouble() * (m_mode == Mode::Streaming ? 1.5 : 0.3), 60.2);
    m_vBitrate = 5800 + int(rng->generateDouble() * 1200);

    if (m_cpu)  m_cpu->setText(QStringLiteral("%1%").arg(qRound(m_vCpu)));
    if (m_ram)  m_ram->setText(QStringLiteral("%1%").arg(qRound(m_vRam)));
    if (m_disk) m_disk->setText(QStringLiteral("%1% · 412 GB free").arg(qRound(m_vDisk)));
    if (m_fps)  m_fps->setText(QString::number(m_vFps, 'f', 1));
    if (m_bitrate) m_bitrate->setText(QStringLiteral("%1 Mb/s").arg(m_vBitrate / 1000.0, 0, 'f', 1));
}

void StudioStatusBar::flashMessage(const QString& text, int ms) {
    if (text.isEmpty()) return;
    m_message->setText(text);
    m_message->setVisible(true);
    m_stateChip->setVisible(false);
    m_messageTimer->start(ms);
}
