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
    explicit AudioMixerPanel(AudioController* controller, QWidget* parent = nullptr);

private slots:
    void rebuild();
    void onLevels(const QString& id, float peakL, float peakR);
    void onConnectionChanged(const QString& id, bool connected);

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
    QVBoxLayout*          m_lanes       = nullptr;
    QHash<QString, Strip> m_strips;

    // Master bus limiter controls (pinned to panel footer).
    QCheckBox* m_limiterToggle    = nullptr;
    QSlider*   m_limiterThreshold = nullptr;
};
