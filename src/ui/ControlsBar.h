#pragma once
#include <QElapsedTimer>
#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QTimer;

class ControlsBar : public QWidget {
    Q_OBJECT
public:
    explicit ControlsBar(QWidget* parent = nullptr);

    enum class TransitionType { Cut, Fade };
    TransitionType transitionType() const;
    int transitionDurationMs() const;

    // Force the record/stream button visuals back to "Start" without emitting
    // any signal. Used by MainWindow when the user cancels or ffmpeg is unavailable.
    void forceStopRecording();
    void forceStopStreaming();

    // Toggle button enabled state. Pass a tooltip to explain why it's disabled.
    void setRecordEnabled(bool enabled, const QString& disabledTooltip = QString());
    void setStreamEnabled(bool enabled, const QString& disabledTooltip = QString());

    // v7 Tier 3: live ffmpeg progress for the streaming pipeline (bitrate in
    // kbps, dropped frames). Updates the stream stats label next to the LIVE
    // timer. Hidden when not streaming.
    void setStreamStats(int bitrateKbps, int droppedFrames);

signals:
    void transitionTypeChanged(TransitionType type);
    void transitionDurationChanged(int ms);
    void recordingStarted();
    void recordingStopped();
    void streamingStarted();
    void streamingStopped();

private slots:
    void onTransitionComboChanged(int index);
    void onRecordClicked();
    void onRecordTick();
    void onStreamClicked();
    void onStreamTick();

private:
    QComboBox*      m_transCombo     = nullptr;
    QDoubleSpinBox* m_transDuration  = nullptr;

    // Recording
    QPushButton*    m_recordBtn      = nullptr;
    QLabel*         m_recordTimer    = nullptr;
    QTimer*         m_recordTicker   = nullptr;
    QElapsedTimer   m_recordElapsed;
    bool            m_recording      = false;

    // Streaming
    QPushButton*    m_streamBtn      = nullptr;
    QLabel*         m_streamTimer    = nullptr;
    QLabel*         m_streamStats    = nullptr;   // Tier 3: "4500 kbps · drops 0" (hidden when idle)
    QTimer*         m_streamTicker   = nullptr;
    QElapsedTimer   m_streamElapsed;
    bool            m_streaming      = false;
};
