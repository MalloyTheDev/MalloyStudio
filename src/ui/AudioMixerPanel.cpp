#include "AudioMixerPanel.h"
#include "MicrophonePickerDialog.h"
#include "VuMeter.h"
#include "audio/AudioController.h"
#include "model/SceneCollection.h"

#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

AudioMixerPanel::AudioMixerPanel(AudioController* controller,
                                 SceneCollection* scenes,
                                 QWidget* parent)
    : QWidget(parent), m_controller(controller), m_scenes(scenes)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 6);
    root->setSpacing(4);

    // ── Header bar with quick-add button ───────────────────────────────────
    // Second entry point for "Add Microphone" — the user can also reach it
    // via Sources → + → Microphone, but a button right above the strips makes
    // the most common audio task discoverable without leaving the mixer.
    auto* headerRow = new QHBoxLayout();
    headerRow->setContentsMargins(0, 0, 0, 0);
    auto* addMicBtn = new QPushButton(tr("+ Add Microphone"), this);
    addMicBtn->setToolTip(tr("Add a microphone capture device to the current scene"));
    if (!m_scenes) addMicBtn->setEnabled(false);   // tests pass nullptr
    connect(addMicBtn, &QPushButton::clicked,
            this,     &AudioMixerPanel::onAddMicrophoneClicked);
    headerRow->addWidget(addMicBtn);
    headerRow->addStretch();
    root->addLayout(headerRow);

    // Empty-state placeholder shown when no audio inputs exist at all
    // (loopback:default is auto-created, so this is rare — but we cover it).
    m_emptyLabel = new QLabel(tr("No audio inputs detected."), this);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setStyleSheet(QStringLiteral("color: #70737a; padding: 8px;"));
    m_emptyLabel->setVisible(false);
    root->addWidget(m_emptyLabel);

    m_lanes = new QVBoxLayout();
    m_lanes->setSpacing(6);
    root->addLayout(m_lanes);
    root->addStretch();

    // ── Master bus limiter ────────────────────────────────────────────────────
    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    root->addWidget(sep);

    auto* masterRow = new QHBoxLayout();
    m_limiterToggle = new QCheckBox(tr("Master limiter"), this);
    m_limiterToggle->setChecked(m_controller->limiterEnabled());
    m_limiterToggle->setToolTip(tr("Soft-knee brickwall limiter on the master bus"));
    masterRow->addWidget(m_limiterToggle);

    auto* threshLabel = new QLabel(tr("Threshold:"), this);
    masterRow->addWidget(threshLabel);

    // Range: -24 dBFS … 0 dBFS (stored as positive int 0..24 = 0…-24 dB).
    m_limiterThreshold = new QSlider(Qt::Horizontal, this);
    m_limiterThreshold->setRange(0, 24);
    // Map stored db (-3 → int 3): invert so 0 dB = slider 0, -24 dB = slider 24.
    const int initThreshInt = static_cast<int>(-m_controller->limiterThresholdDb());
    m_limiterThreshold->setValue(std::clamp(initThreshInt, 0, 24));
    m_limiterThreshold->setMinimumWidth(80);
    m_limiterThreshold->setToolTip(tr("Limiter threshold (0 = 0 dBFS, 24 = -24 dBFS)"));
    masterRow->addWidget(m_limiterThreshold, 1);

    auto* dbLabel = new QLabel(this);
    dbLabel->setText(QStringLiteral("-%1 dB").arg(m_limiterThreshold->value()));
    masterRow->addWidget(dbLabel);
    root->addLayout(masterRow);

    connect(m_limiterToggle, &QCheckBox::toggled, this, [this](bool on) {
        m_controller->setLimiterEnabled(on);
    });
    connect(m_limiterThreshold, &QSlider::valueChanged, this, [this, dbLabel](int v) {
        dbLabel->setText(v == 0 ? tr("0 dB") : QStringLiteral("-%1 dB").arg(v));
        m_controller->setLimiterThresholdDb(static_cast<float>(-v));
    });

    connect(m_controller, &AudioController::inputsChanged,
            this, &AudioMixerPanel::rebuild);
    connect(m_controller, &AudioController::levelsUpdated,
            this, &AudioMixerPanel::onLevels);
    connect(m_controller, &AudioController::inputConnectionChanged,
            this, &AudioMixerPanel::onConnectionChanged);

    rebuild();
}

void AudioMixerPanel::rebuild() {
    // Remove existing strips
    for (auto it = m_strips.begin(); it != m_strips.end(); ++it)
        delete it.value().root;
    m_strips.clear();

    for (const AudioInput& in : m_controller->inputs()) {
        Strip s = makeStrip(in.id, in);
        m_lanes->addWidget(s.root);
        m_strips.insert(in.id, s);
        s.meter->setEnabled(in.connected);
    }

    if (m_emptyLabel) m_emptyLabel->setVisible(m_strips.isEmpty());
}

void AudioMixerPanel::onAddMicrophoneClicked() {
    if (!m_scenes || !m_controller) return;
    // Reuse the same MicrophonePickerDialog the Sources panel uses, so both
    // entry points feel consistent — same device list, same selection UX.
    const auto pick = MicrophonePickerDialog::pick(m_controller, this);
    if (pick.first.isEmpty()) return;   // cancelled / no devices
    // Use the friendly device name as the source name by default. Users can
    // rename later via the Inspector or inline rename in the Sources panel.
    m_scenes->addAudioInputToCurrent(pick.second, pick.first);
}

AudioMixerPanel::Strip AudioMixerPanel::makeStrip(const QString& id, const AudioInput& in) {
    Strip s;
    s.root = new QWidget(this);
    auto* row = new QHBoxLayout(s.root);
    row->setContentsMargins(4, 2, 4, 2);
    row->setSpacing(8);

    s.name = new QLabel(in.name, s.root);
    s.name->setMinimumWidth(100);
    QFont f = s.name->font();
    f.setBold(true);
    s.name->setFont(f);

    s.meter = new VuMeter(s.root);
    // Compact minimums: this strip lives in a ~340px-wide sidebar rail, so the
    // widths must sum to less than that or the QHBoxLayout compresses past the
    // minimums and the sliders overlap the name label. The meter/volume stretch
    // to fill whatever space remains, so they still grow on a wider layout.
    s.meter->setMinimumWidth(36);

    s.volume = new QSlider(Qt::Horizontal, s.root);
    s.volume->setRange(0, 150); // 0..150 maps to 0.0..1.5
    s.volume->setValue(static_cast<int>(in.volume * 100.0f));
    s.volume->setMinimumWidth(48);
    s.volume->setToolTip(tr("Volume (0–150 %)"));

    // Pan: -100..+100 maps to -1.0..+1.0; tick at center.
    s.pan = new QSlider(Qt::Horizontal, s.root);
    s.pan->setRange(-100, 100);
    s.pan->setValue(static_cast<int>(in.pan * 100.0f));
    s.pan->setFixedWidth(52);
    s.pan->setToolTip(tr("Pan  ◄ L──C──R ►"));
    s.pan->setTickPosition(QSlider::TicksBelow);
    s.pan->setTickInterval(100);

    s.mute = new QToolButton(s.root);
    s.mute->setText(tr("M"));
    s.mute->setCheckable(true);
    s.mute->setChecked(in.muted);
    s.mute->setToolTip(tr("Mute"));
    s.mute->setFixedWidth(28);

    row->addWidget(s.name);
    row->addWidget(s.meter, 1);
    row->addWidget(s.volume, 1);
    row->addWidget(s.pan);
    row->addWidget(s.mute);

    connect(s.volume, &QSlider::valueChanged, this, [this, id](int v) {
        m_controller->setVolume(id, static_cast<float>(v) / 100.0f);
    });
    connect(s.pan, &QSlider::valueChanged, this, [this, id](int v) {
        m_controller->setPan(id, static_cast<float>(v) / 100.0f);
    });
    connect(s.mute, &QToolButton::toggled, this, [this, id](bool muted) {
        m_controller->setMuted(id, muted);
    });

    return s;
}

void AudioMixerPanel::onLevels(const QString& id, float peakL, float peakR) {
    auto it = m_strips.find(id);
    if (it == m_strips.end()) return;
    it.value().meter->setLevels(peakL, peakR);
}

void AudioMixerPanel::onConnectionChanged(const QString& id, bool connected) {
    auto it = m_strips.find(id);
    if (it == m_strips.end()) return;
    it.value().meter->setEnabled(connected);
    it.value().volume->setEnabled(connected);
    it.value().pan->setEnabled(connected);
    it.value().mute->setEnabled(connected);
    it.value().name->setStyleSheet(connected
        ? QString()
        : QStringLiteral("color: #70737a; font-style: italic;"));
}
