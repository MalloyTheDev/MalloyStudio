#pragma once

#include "ICaptureSource.h"

#include <QObject>
#include <QHash>
#include <QImage>
#include <QSet>
#include <QString>
#include <functional>

class SceneCollection;
class DxgiCapture;

class CaptureSession : public QObject {
    Q_OBJECT
public:
    explicit CaptureSession(QObject* parent = nullptr) : QObject(parent) {}
    ~CaptureSession() override = default;

    virtual void startCapture() = 0;
    virtual void stopCapture() = 0;

    // What this session's backend has produced and lost, for the SOURCE RX and
    // CAP DROP rows. Sessions whose backend does not count report nothing
    // rather than zero pretending to be a measurement, which is what the
    // default here is: a camera or a PrintWindow capture has no such number,
    // and inventing one would put a confident 0 next to a real drop count.
    virtual CaptureStats stats() const { return {}; }

    // Whether anything downstream wants frames from this session.
    //
    // Suspending delivery is not stopping the session: the backend keeps its
    // handle on the desktop and its statistics, and resuming costs nothing.
    // Backends that cannot suspend simply keep producing, which is what they
    // did before.
    virtual void setDelivering(bool) {}

signals:
    void frameReady(QImage frame);
    void captureError(QString message);
};

class DxgiCaptureSession final : public CaptureSession {
    Q_OBJECT
public:
    DxgiCaptureSession(int adapterIndex, int outputIndex, QObject* parent = nullptr);
    ~DxgiCaptureSession() override;

    void startCapture() override;
    void stopCapture() override;
    CaptureStats stats() const override;
    void setDelivering(bool delivering) override;

private:
    bool m_delivering = true;
    int m_adapterIndex = -1;
    int m_outputIndex = -1;
    DxgiCapture* m_capture = nullptr;
    // The backend's counters, kept once it has been torn down: see
    // WgcCaptureSession for why a stopped session still has to be able to say
    // what it produced.
    CaptureStats m_retired;
};

class CaptureController : public QObject {
    Q_OBJECT
public:
    using SessionFactory       = std::function<CaptureSession*(int adapterIndex, int outputIndex, QObject* parent)>;
    using WindowSessionFactory = std::function<CaptureSession*(quintptr hwnd, QObject* parent)>;
    using CameraSessionFactory = std::function<CaptureSession*(const QString& deviceId, QObject* parent)>;

    explicit CaptureController(SceneCollection* scenes, QObject* parent = nullptr);
    CaptureController(SceneCollection* scenes, SessionFactory factory, QObject* parent = nullptr);
    ~CaptureController() override;

    QString statusSummary() const { return m_statusSummary; }
    QString monitorStatus(int adapterIndex, int outputIndex) const;
    // Returns display session count (for tests / status bar compatibility).
    int activeSessionCount() const { return m_sessions.size(); }
    int activeWindowSessionCount() const { return m_windowSessions.size(); }

    // Every display and window backend's production and loss for this run,
    // including sessions that have already been stopped.
    //
    // Summed across sessions and across time on purpose. A source that was
    // switched away from still lost the frames it lost, and a reconcile that
    // rebuilds a session must not reset the account of what a recording
    // captured.
    //
    // Never reset. A consumer that wants one run's figures takes a reading at
    // the start and subtracts, which is what EncoderPipeline does: a counter
    // that can be zeroed by one consumer is a counter no other consumer can
    // trust.
    CaptureStats captureStats() const;

    // Whether any consumer wants captured frames at all.
    //
    // With no recording, no stream and no visible preview, capture was still
    // performing every readback and discarding the result: about 56 a second
    // at 3 to 4.7 ms each, a fifth of a processor core for nobody. Sessions
    // stay alive and keep their statistics; they simply stop producing, so
    // this is not a recording epoch boundary and nothing downstream is reset.
    void setDelivering(bool delivering);
    bool isDelivering() const { return m_delivering; }

    static QString keyFor(int adapterIndex, int outputIndex);
    static QString keyForWindow(quintptr hwnd);

public slots:
    void reconcile();
    void stopAll();

signals:
    // Display capture signals (unchanged).
    void frameReady(int adapterIndex, int outputIndex, QImage frame);
    void frameCleared(int adapterIndex, int outputIndex);
    void statusChanged(QString summary);
    void monitorStatusChanged(int adapterIndex, int outputIndex, QString status);

    // Window capture signals.
    void windowFrameReady(quintptr hwnd, QImage frame);
    void windowFrameCleared(quintptr hwnd);

    // Camera capture signals (keyed by MF device id).
    void cameraFrameReady(QString deviceId, QImage frame);
    void cameraFrameCleared(QString deviceId);

private:
    struct ActiveSession {
        int adapterIndex = -1;
        int outputIndex = -1;
        CaptureSession* session = nullptr;
        QString status;
    };
    struct ActiveWindowSession {
        quintptr        hwnd    = 0;
        CaptureSession* session = nullptr;
    };
    struct ActiveCameraSession {
        QString         deviceId;
        CaptureSession* session = nullptr;
    };

    void startSession(int adapterIndex, int outputIndex);
    void stopSession(const QString& key, bool setIdleStatus = true);
    void startWindowSession(quintptr hwnd);
    void stopWindowSession(const QString& key);
    void startCameraSession(const QString& deviceId);
    void stopCameraSession(const QString& deviceId);
    void setSummary(const QString& summary);
    void setMonitorStatus(const QString& key, const QString& status);

    SceneCollection*  m_scenes = nullptr;
    SessionFactory    m_factory;
    WindowSessionFactory m_windowFactory;
    CameraSessionFactory m_cameraFactory;
    QHash<QString, ActiveSession>       m_sessions;
    QHash<QString, ActiveWindowSession> m_windowSessions;
    QHash<QString, ActiveCameraSession> m_cameraSessions;
    // Backend counters from sessions that have already been stopped, so a run
    // summary can still account for the frames they produced.
    CaptureStats m_retiredStats;
    bool m_delivering = true;

    QHash<QString, QString> m_lastStatus;
    QSet<QString> m_blockedErrorKeys;
    QString m_statusSummary = QStringLiteral("Idle");
};
