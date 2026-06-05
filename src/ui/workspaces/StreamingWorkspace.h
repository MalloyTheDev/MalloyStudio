#pragma once

#include "audio/AudioInput.h"
#include "recording/StreamSettings.h"

#include <QHash>
#include <QString>
#include <QWidget>

class AudioController;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QToolButton;
class QTimer;
class QVBoxLayout;
class VuMeter;

// Streaming Studio (secondary.jsx StreamStudio): destination + preview +
// go-live card in the center, and a right rail with stream health, chat,
// alerts and a real mix. Mix levels + volume/mute come from the live
// AudioController (the same instance the Recording workspace uses), so the
// two views stay in lockstep: changes here are visible in the Recording
// mixer and vice versa. Live stream telemetry (viewers/bitrate/dropped/ping)
// is still simulated. Emits goLiveRequested() so MainWindow drives the real
// pipeline.
class StreamingWorkspace : public QWidget {
    Q_OBJECT
public:
    // audio must be the same AudioController the Recording workspace observes,
    // so the two mix views share state. Asserted non-null in the ctor.
    explicit StreamingWorkspace(AudioController* audio, QWidget* parent = nullptr);

    void setLive(bool live);

signals:
    void goLiveRequested();

protected:
    void showEvent(QShowEvent* event) override;   // run telemetry tick only while visible
    void hideEvent(QHideEvent* event) override;

private slots:
    void rebuildMixStrips();
    void onMixLevels(const QString& id, float peakL, float peakR);
    void onMixConnectionChanged(const QString& id, bool connected);
    void onMixControlChanged(const QString& id);

private:
    QWidget* buildCenter();
    QWidget* buildRail();
    void tick();          // simulates live telemetry only (levels are real)
    void loadMeta();      // populate fields from StreamSettings
    void persistMeta();   // write title/category/tags back to StreamSettings

    // One channel strip per AudioController input (loopback + every mic).
    struct Strip {
        QWidget*     root   = nullptr;
        QLabel*      name   = nullptr;
        VuMeter*     meter  = nullptr;
        QSlider*     volume = nullptr;
        QToolButton* mute   = nullptr;
    };
    Strip makeMixStrip(const QString& id, const AudioInput& in);

    AudioController*      m_audio = nullptr;
    QVBoxLayout*          m_mixLanes = nullptr;
    QLabel*               m_mixEmpty = nullptr;
    QHash<QString, Strip> m_mixStrips;

    StreamSettings m_settings;
    QLineEdit*   m_titleEdit = nullptr;
    QComboBox*   m_catCombo = nullptr;
    QPushButton* m_destBtn = nullptr;
    QWidget*     m_tagsHost = nullptr;

    bool m_live = false;
    int  m_elapsed = 0;
    int  m_viewers = 0;
    int  m_dropped = 0;

    QLabel*    m_liveTag = nullptr;
    QLabel*    m_statusTag = nullptr;
    QLabel*    m_mViewers = nullptr;
    QLabel*    m_mBitrate = nullptr;
    QLabel*    m_mDropped = nullptr;
    QLabel*    m_mPing = nullptr;
    QLabel*    m_mFps = nullptr;
    QPushButton* m_goLive = nullptr;
    QWidget*   m_chatEmpty = nullptr;
    QWidget*   m_chatList = nullptr;
    QWidget*   m_alertsEmpty = nullptr;
    QWidget*   m_alertsList = nullptr;
    QLineEdit* m_chatInput = nullptr;
    QTimer*    m_timer = nullptr;
};
