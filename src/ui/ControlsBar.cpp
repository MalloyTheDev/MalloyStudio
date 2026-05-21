#include "ControlsBar.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>

ControlsBar::ControlsBar(QWidget* parent) : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(6);

    // --- Transition picker ---
    layout->addWidget(new QLabel(tr("Transition:"), this));

    m_transCombo = new QComboBox(this);
    m_transCombo->addItem(tr("Cut"));
    m_transCombo->addItem(tr("Fade"));
    m_transCombo->setFixedWidth(80);
    layout->addWidget(m_transCombo);

    m_transDuration = new QDoubleSpinBox(this);
    m_transDuration->setRange(0.1, 30.0);
    m_transDuration->setSingleStep(0.5);
    m_transDuration->setValue(0.5);
    m_transDuration->setSuffix(tr(" s"));
    m_transDuration->setDecimals(1);
    m_transDuration->setFixedWidth(72);
    m_transDuration->setVisible(false); // hidden for "Cut"
    layout->addWidget(m_transDuration);

    layout->addStretch();

    // --- Stream button + timer (left of record so it appears first) ---
    m_streamTimer = new QLabel(QStringLiteral(""), this);
    m_streamTimer->setVisible(false);
    m_streamTimer->setStyleSheet(QStringLiteral("color: #ff625f; font-weight: bold;"));
    layout->addWidget(m_streamTimer);

    // Live ffmpeg progress shown next to the timer while streaming. Smaller +
    // dimmer than the timer so it reads as supplementary detail, not status.
    m_streamStats = new QLabel(QStringLiteral(""), this);
    m_streamStats->setVisible(false);
    m_streamStats->setStyleSheet(QStringLiteral(
        "color: #70737a; font-size: 8pt; padding-left: 4px;"));
    layout->addWidget(m_streamStats);

    m_streamBtn = new QPushButton(tr("⬛  Start Stream"), this);
    m_streamBtn->setMinimumWidth(130);
    layout->addWidget(m_streamBtn);

    // --- Recording button + timer ---
    m_recordTimer = new QLabel(QStringLiteral(""), this);
    m_recordTimer->setVisible(false);
    m_recordTimer->setStyleSheet(QStringLiteral("color: #ff625f; font-weight: bold;"));
    layout->addWidget(m_recordTimer);

    m_recordBtn = new QPushButton(tr("●  Start Recording"), this);
    m_recordBtn->setMinimumWidth(140);
    layout->addWidget(m_recordBtn);

    // --- Connections ---
    connect(m_transCombo, &QComboBox::currentIndexChanged,
            this, &ControlsBar::onTransitionComboChanged);
    connect(m_transDuration, &QDoubleSpinBox::valueChanged, this, [this](double) {
        emit transitionDurationChanged(transitionDurationMs());
    });
    connect(m_recordBtn, &QPushButton::clicked, this, &ControlsBar::onRecordClicked);
    connect(m_streamBtn, &QPushButton::clicked, this, &ControlsBar::onStreamClicked);
}

ControlsBar::TransitionType ControlsBar::transitionType() const {
    return m_transCombo->currentIndex() == 1 ? TransitionType::Fade : TransitionType::Cut;
}

int ControlsBar::transitionDurationMs() const {
    return static_cast<int>(m_transDuration->value() * 1000.0);
}

void ControlsBar::onTransitionComboChanged(int index) {
    m_transDuration->setVisible(index == 1);
    emit transitionTypeChanged(index == 1 ? TransitionType::Fade : TransitionType::Cut);
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

void ControlsBar::toggleRecord() {
    if (m_recordBtn && m_recordBtn->isEnabled()) onRecordClicked();
}

void ControlsBar::toggleStream() {
    if (m_streamBtn && m_streamBtn->isEnabled()) onStreamClicked();
}

void ControlsBar::onRecordClicked() {
    if (!m_recording) {
        m_recording = true;
        m_recordElapsed.start();
        m_recordTimer->setText(QStringLiteral("00:00:00"));
        m_recordTimer->setVisible(true);
        m_recordBtn->setText(tr("■  Stop Recording"));

        m_recordTicker = new QTimer(this);
        m_recordTicker->setInterval(1000);
        connect(m_recordTicker, &QTimer::timeout, this, &ControlsBar::onRecordTick);
        m_recordTicker->start();

        emit recordingStarted();
    } else {
        forceStopRecording();
        emit recordingStopped();
    }
}

void ControlsBar::forceStopRecording() {
    if (!m_recording) return;
    m_recording = false;
    if (m_recordTicker) {
        m_recordTicker->stop();
        m_recordTicker->deleteLater();
        m_recordTicker = nullptr;
    }
    m_recordTimer->setVisible(false);
    m_recordBtn->setText(tr("●  Start Recording"));
}

void ControlsBar::setRecordEnabled(bool enabled, const QString& disabledTooltip) {
    m_recordBtn->setEnabled(enabled);
    m_recordBtn->setToolTip(enabled ? QString() : disabledTooltip);
}

void ControlsBar::onRecordTick() {
    const qint64 secs = m_recordElapsed.elapsed() / 1000;
    const int h = static_cast<int>(secs / 3600);
    const int m = static_cast<int>((secs % 3600) / 60);
    const int s = static_cast<int>(secs % 60);
    m_recordTimer->setText(QStringLiteral("%1:%2:%3")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0')));
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

void ControlsBar::onStreamClicked() {
    if (!m_streaming) {
        m_streaming = true;
        m_streamElapsed.start();
        m_streamTimer->setText(QStringLiteral("00:00:00"));
        m_streamTimer->setVisible(true);
        m_streamBtn->setText(tr("■  Stop Stream"));

        m_streamTicker = new QTimer(this);
        m_streamTicker->setInterval(1000);
        connect(m_streamTicker, &QTimer::timeout, this, &ControlsBar::onStreamTick);
        m_streamTicker->start();

        emit streamingStarted();
    } else {
        forceStopStreaming();
        emit streamingStopped();
    }
}

void ControlsBar::forceStopStreaming() {
    if (!m_streaming) return;
    m_streaming = false;
    if (m_streamTicker) {
        m_streamTicker->stop();
        m_streamTicker->deleteLater();
        m_streamTicker = nullptr;
    }
    m_streamTimer->setVisible(false);
    if (m_streamStats) {
        m_streamStats->setVisible(false);
        m_streamStats->clear();
    }
    m_streamBtn->setText(tr("⬛  Start Stream"));
}

void ControlsBar::setStreamStats(int bitrateKbps, int droppedFrames) {
    if (!m_streaming || !m_streamStats) return;
    m_streamStats->setText(QStringLiteral("· %1 kbps · drops %2")
                               .arg(bitrateKbps)
                               .arg(droppedFrames));
    m_streamStats->setVisible(true);
}

void ControlsBar::setStreamEnabled(bool enabled, const QString& disabledTooltip) {
    m_streamBtn->setEnabled(enabled);
    m_streamBtn->setToolTip(enabled ? QString() : disabledTooltip);
}

void ControlsBar::onStreamTick() {
    const qint64 secs = m_streamElapsed.elapsed() / 1000;
    const int h = static_cast<int>(secs / 3600);
    const int m = static_cast<int>((secs % 3600) / 60);
    const int s = static_cast<int>(secs % 60);
    m_streamTimer->setText(QStringLiteral("LIVE %1:%2:%3")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0')));
}
