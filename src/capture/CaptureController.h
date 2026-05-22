#pragma once

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

private:
    int m_adapterIndex = -1;
    int m_outputIndex = -1;
    DxgiCapture* m_capture = nullptr;
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
    QHash<QString, QString> m_lastStatus;
    QSet<QString> m_blockedErrorKeys;
    QString m_statusSummary = QStringLiteral("Idle");
};
