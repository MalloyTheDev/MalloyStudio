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
class QVBoxLayout;
class VuMeter;

// Streaming Studio: destination + preview + go-live card in the center, and a
// right rail with stream health, chat, alerts and a real mix. Mix levels +
// volume/mute come from the live AudioController (the same instance the
// Recording workspace uses), so the two views stay in lockstep: changes here
// are visible in the Recording mixer and vice versa. Emits goLiveRequested()
// so MainWindow drives the real pipeline.
//
// Every figure shown here is measured or left blank. Bitrate and dropped
// frames come from onStreamProgress(), which MainWindow drives from ffmpeg's
// progress lines. Viewer count has no source until the channel is queried, and
// says so rather than showing a number. Chat and alerts have no source at all
// yet, so their panels stay empty.
class StreamingWorkspace : public QWidget {
    Q_OBJECT
public:
    // audio must be the same AudioController the Recording workspace observes,
    // so the two mix views share state. Asserted non-null in the ctor.
    explicit StreamingWorkspace(AudioController* audio, QWidget* parent = nullptr);

    void setLive(bool live);

public slots:
    // Measured bitrate and dropped-frame count from the running stream, driven
    // by MediaController::streamingProgress at roughly 1 Hz.
    void onStreamProgress(int bitrateKbps, int droppedFrames);

signals:
    void goLiveRequested();

private slots:
    void rebuildMixStrips();
    void onMixLevels(const QString& id, float peakL, float peakR);
    void onMixConnectionChanged(const QString& id, bool connected);
    void onMixControlChanged(const QString& id);

private:
    QWidget* buildCenter();
    QWidget* buildRail();
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
    int  m_dropped = 0;

    QLabel*    m_liveTag = nullptr;
    QLabel*    m_statusTag = nullptr;
    QLabel*    m_mViewers = nullptr;
    QLabel*    m_mBitrate = nullptr;
    QLabel*    m_mDropped = nullptr;
    QLabel*    m_mEncoder = nullptr;
    QLabel*    m_keyTag = nullptr;
    QPushButton* m_goLive = nullptr;
    QWidget*   m_chatEmpty = nullptr;
    QLabel*    m_alertsEmpty = nullptr;
    QLineEdit* m_chatInput = nullptr;
};
