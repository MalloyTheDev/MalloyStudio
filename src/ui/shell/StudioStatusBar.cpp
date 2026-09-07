#include "ui/shell/StudioStatusBar.h"
#include "project/ByteSize.h"
#include "project/RecentRecordings.h"
#include "ui/Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QStorageInfo>
#include <QTimer>

namespace {
// Shown for any figure the app cannot currently read.
const QString kNoValue = QStringLiteral("-");

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
    // Encode rate and bitrate only exist while ffmpeg is running. They stay in
    // place and read as unknown the rest of the time, rather than appearing and
    // disappearing: a bar whose contents move around is harder to read at a
    // glance than one with a steady shape.
    row->addWidget(makeStat(QStringLiteral("FPS"), &m_fps));
    row->addWidget(makeStat(QStringLiteral("BITRATE"), &m_bitrate));
    // Losing frames is not the normal case, so this stays out of the way until
    // it happens rather than sitting at zero and training the eye to skip it.
    m_dropStat = makeStat(QStringLiteral("ENC DROP"), &m_drops);
    m_dropStat->setVisible(false);
    row->addWidget(m_dropStat);

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
    // Encoder figures belong to the run that produced them, so clear them on
    // every mode change. setEncodeStats() fills them in once ffmpeg reports,
    // and a stale number from the previous run is never left on screen.
    if (m_bitrate) m_bitrate->setText(kNoValue);
    if (m_fps)     m_fps->setText(kNoValue);
    if (m_dropStat) m_dropStat->setVisible(false);
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
    // Processor load is the difference between two readings, so the first tick
    // after startup has nothing to compare against and says so.
    const MachineLoad::CpuSample now = MachineLoad::readCpu();
    const double cpu = MachineLoad::cpuBusyPercent(m_lastCpu, now);
    m_lastCpu = now;
    if (m_cpu)
        m_cpu->setText(cpu < 0 ? kNoValue : QStringLiteral("%1%").arg(qRound(cpu)));

    const double ram = MachineLoad::memoryUsedPercent();
    if (m_ram)
        m_ram->setText(ram < 0 ? kNoValue : QStringLiteral("%1%").arg(qRound(ram)));

    // The volume recordings are written to, which is the one the user runs out
    // of room on. The same volume SystemProbe reports in the settings page, so
    // the two figures agree.
    if (m_disk) {
        const QStorageInfo storage(RecentRecordings::outputDir());
        if (storage.isValid() && storage.isReady() && storage.bytesTotal() > 0) {
            const qint64 free = storage.bytesAvailable();
            const int usedPct = int(100.0 * double(storage.bytesTotal() - free)
                                            / double(storage.bytesTotal()) + 0.5);
            m_disk->setText(tr("%1% · %2 free").arg(usedPct).arg(formatByteSize(free)));
        } else {
            m_disk->setText(kNoValue);
        }
    }
}

void StudioStatusBar::setEncodeStats(int bitrateKbps, int droppedFrames, int encodeFps,
                                     int composedFramesRejected) {
    Q_UNUSED(droppedFrames);   // ffmpeg's own count, shown by ControlsBar

    // ffmpeg reports several times a second and often omits a field on its
    // first lines. Only write when the text actually changes, so the bar is not
    // relaid out on every line for a value that did not move.
    auto setIfChanged = [](QLabel* label, const QString& text) {
        if (label && label->text() != text) label->setText(text);
    };

    setIfChanged(m_bitrate, bitrateKbps > 0
                                ? tr("%1 Mb/s").arg(bitrateKbps / 1000.0, 0, 'f', 1)
                                : kNoValue);
    setIfChanged(m_fps, encodeFps > 0 ? QString::number(encodeFps) : kNoValue);

    // Pictures this machine composed and could not hand to the encoder. Named
    // ENC DROP rather than DROPPED so it cannot be confused with frames a
    // capture backend lost before composition, which would read CAP DROP.
    // Silent frame loss is its own dishonesty, so once it starts the count
    // stays on screen.
    if (m_dropStat && m_drops) {
        if (composedFramesRejected > 0) {
            m_dropStat->setVisible(true);
            setIfChanged(m_drops, QString::number(composedFramesRejected));
            Theme::setTone(m_drops, QStringLiteral("warn"));
        }
    }
}

void StudioStatusBar::flashMessage(const QString& text, int ms) {
    if (text.isEmpty()) return;
    m_message->setText(text);
    m_message->setVisible(true);
    m_stateChip->setVisible(false);
    m_messageTimer->start(ms);
}
