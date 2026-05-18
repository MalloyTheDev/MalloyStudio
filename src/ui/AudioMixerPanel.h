#pragma once
#include "audio/AudioInput.h"

#include <QHash>
#include <QString>
#include <QWidget>

class AudioController;
class QCheckBox;
class QLabel;
class QSlider;
class QToolButton;
class QVBoxLayout;
class SceneCollection;
class VuMeter;

// Mixer dock content. Hosts one channel strip per AudioController input.
//
// Layout per strip (horizontal):
//   [name]  [▮▮▮ VU meter ▮▮▮]  [── volume ──]  [◄pan►]  [M]
//
// Footer row: [Master limiter ☐]  [threshold ▸▸▸]
class AudioMixerPanel : public QWidget {
    Q_OBJECT
public:
    // SceneCollection is needed so the "+ Add Microphone" button at the top
    // of the mixer can call addAudioInputToCurrent() on the current scene.
    // Passing nullptr disables that button (used only by tests).
    explicit AudioMixerPanel(AudioController* controller,
                             SceneCollection* scenes,
                             QWidget* parent = nullptr);

private slots:
    void rebuild();
    void onLevels(const QString& id, float peakL, float peakR);
    void onConnectionChanged(const QString& id, bool connected);
    void onAddMicrophoneClicked();

private:
    struct Strip {
        QWidget*     root   = nullptr;
        QLabel*      name   = nullptr;
        VuMeter*     meter  = nullptr;
        QSlider*     volume = nullptr;
        QSlider*     pan    = nullptr;
        QToolButton* mute   = nullptr;
    };

    Strip makeStrip(const QString& id, const AudioInput& in);

    AudioController*      m_controller  = nullptr;
    SceneCollection*      m_scenes      = nullptr;
    QVBoxLayout*          m_lanes       = nullptr;
    QHash<QString, Strip> m_strips;
    QLabel*               m_emptyLabel  = nullptr;  // shown when no mixer strips exist (Tier 3)

    // Master bus limiter controls (pinned to panel footer).
    QCheckBox* m_limiterToggle    = nullptr;
    QSlider*   m_limiterThreshold = nullptr;
};
