#pragma once

#include "recording/StreamSettings.h"

#include <QWidget>

class MeterBar;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

// Streaming Studio (secondary.jsx StreamStudio): destination + preview +
// go-live card in the center, and a right rail with stream health, chat,
// alerts and a mix. Visuals switch between offline and live; live telemetry
// is simulated. Emits goLiveRequested() so MainWindow drives the real pipeline.
class StreamingWorkspace : public QWidget {
    Q_OBJECT
public:
    explicit StreamingWorkspace(QWidget* parent = nullptr);

    void setLive(bool live);

signals:
    void goLiveRequested();

protected:
    void showEvent(QShowEvent* event) override;   // run meters only while visible
    void hideEvent(QHideEvent* event) override;

private:
    QWidget* buildCenter();
    QWidget* buildRail();
    void tick();
    void loadMeta();      // populate fields from StreamSettings
    void persistMeta();   // write title/category/tags back to StreamSettings

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
    MeterBar*  m_micMeter = nullptr;
    MeterBar*  m_deskMeter = nullptr;
    QTimer*    m_timer = nullptr;

    double m_micL = 0.4, m_micP = 0.4, m_deskL = 0.55, m_deskP = 0.55;
};
