#include "CaptureController.h"
#include "DxgiCapture.h"
#include "WindowCaptureSession.h"
#include "model/Scene.h"
#include "model/SceneCollection.h"
#include "model/SceneItem.h"
#include "model/Source.h"

#include <QSet>
#include <utility>

DxgiCaptureSession::DxgiCaptureSession(int adapterIndex, int outputIndex, QObject* parent)
    : CaptureSession(parent), m_adapterIndex(adapterIndex), m_outputIndex(outputIndex)
{}

DxgiCaptureSession::~DxgiCaptureSession() {
    stopCapture();
}

void DxgiCaptureSession::startCapture() {
    if (m_capture) return;
    m_capture = new DxgiCapture(m_adapterIndex, m_outputIndex, this);
    connect(m_capture, &DxgiCapture::frameReady, this, &CaptureSession::frameReady, Qt::QueuedConnection);
    connect(m_capture, &DxgiCapture::captureError, this, &CaptureSession::captureError, Qt::QueuedConnection);
    m_capture->start();
}

void DxgiCaptureSession::stopCapture() {
    if (!m_capture) return;
    disconnect(m_capture, nullptr, this, nullptr);
    m_capture->requestStop();
    m_capture->wait(4000);
    delete m_capture;
    m_capture = nullptr;
}

CaptureController::CaptureController(SceneCollection* scenes, QObject* parent)
    : CaptureController(
          scenes,
          [](int adapterIndex, int outputIndex, QObject* parent) {
              return new DxgiCaptureSession(adapterIndex, outputIndex, parent);
          },
          parent)
{}

CaptureController::CaptureController(SceneCollection* scenes, SessionFactory factory, QObject* parent)
    : QObject(parent), m_scenes(scenes), m_factory(std::move(factory)),
      m_windowFactory([](quintptr hwnd, QObject* p){ return new WindowCaptureSession(hwnd, p); })
{
    connect(m_scenes, &SceneCollection::currentChanged, this, &CaptureController::reconcile);
    connect(m_scenes, &SceneCollection::itemsChanged, this, &CaptureController::reconcile);
    connect(m_scenes, &SceneCollection::sourceChanged, this, [this](int){ reconcile(); });
    connect(m_scenes, &SceneCollection::sourcesChanged, this, &CaptureController::reconcile);
    connect(m_scenes, &SceneCollection::collectionReset, this, &CaptureController::reconcile);
    reconcile();
}

CaptureController::~CaptureController() {
    stopAll();
}

QString CaptureController::keyFor(int adapterIndex, int outputIndex) {
    return QStringLiteral("%1:%2").arg(adapterIndex).arg(outputIndex);
}

QString CaptureController::keyForWindow(quintptr hwnd) {
    return QStringLiteral("window:0x%1").arg(hwnd, 8, 16, QLatin1Char('0'));
}

QString CaptureController::monitorStatus(int adapterIndex, int outputIndex) const {
    return m_lastStatus.value(keyFor(adapterIndex, outputIndex), QStringLiteral("Idle"));
}

void CaptureController::reconcile() {
    QSet<QString> required;
    QSet<QString> requiredWindows;

    if (Scene* scene = m_scenes ? m_scenes->currentScene() : nullptr) {
        for (int i = 0; i < scene->itemCount(); ++i) {
            SceneItem* item = scene->itemAt(i);
            Source* source = m_scenes->sourceForItem(item);
            if (!item->isVisible() || !source) continue;

            if (source->type() == Source::Type::DisplayCapture && source->hasMonitorConfig()) {
                required.insert(keyFor(source->adapterIndex(), source->outputIndex()));
            } else if (source->type() == Source::Type::WindowCapture && source->hasWindowConfig()) {
                requiredWindows.insert(keyForWindow(source->windowHandle()));
            }
        }
    }

    // --- Display sessions ---
    const QList<QString> activeKeys = m_sessions.keys();
    for (const QString& key : activeKeys) {
        if (!required.contains(key)) stopSession(key);
    }

    const QList<QString> blockedKeys = m_blockedErrorKeys.values();
    for (const QString& key : blockedKeys) {
        if (!required.contains(key)) {
            m_blockedErrorKeys.remove(key);
            setMonitorStatus(key, QStringLiteral("Idle"));
        }
    }

    bool blockedRequired = false;
    for (const QString& key : required) {
        if (m_sessions.contains(key)) continue;
        if (m_blockedErrorKeys.contains(key)) {
            blockedRequired = true;
            continue;
        }
        const QStringList parts = key.split(QLatin1Char(':'));
        if (parts.size() != 2) continue;
        startSession(parts.at(0).toInt(), parts.at(1).toInt());
    }

    // --- Window sessions ---
    const QList<QString> activeWindowKeys = m_windowSessions.keys();
    for (const QString& key : activeWindowKeys) {
        if (!requiredWindows.contains(key)) stopWindowSession(key);
    }
    for (const QString& key : requiredWindows) {
        if (m_windowSessions.contains(key)) continue;
        // Parse HWND from "window:0x<hex>".
        const QString hexPart = key.mid(9);   // skip "window:0x"
        bool ok = false;
        const quintptr hwnd = static_cast<quintptr>(hexPart.toULongLong(&ok, 16));
        if (ok && hwnd) startWindowSession(hwnd);
    }

    const int total = m_sessions.size() + m_windowSessions.size();
    if (total == 0) {
        setSummary(blockedRequired ? QStringLiteral("Capture error") : QStringLiteral("Idle"));
    } else {
        setSummary(QStringLiteral("LIVE: %1 source%2")
            .arg(total)
            .arg(total == 1 ? QString() : QStringLiteral("s")));
    }
}

void CaptureController::stopAll() {
    const QList<QString> keys = m_sessions.keys();
    for (const QString& key : keys) stopSession(key);
    const QList<QString> windowKeys = m_windowSessions.keys();
    for (const QString& key : windowKeys) stopWindowSession(key);
    m_blockedErrorKeys.clear();
    setSummary(QStringLiteral("Idle"));
}

void CaptureController::startSession(int adapterIndex, int outputIndex) {
    const QString key = keyFor(adapterIndex, outputIndex);
    CaptureSession* session = m_factory(adapterIndex, outputIndex, this);
    ActiveSession active{adapterIndex, outputIndex, session, QStringLiteral("Starting")};
    m_sessions.insert(key, active);
    setMonitorStatus(key, QStringLiteral("Starting"));

    connect(session, &CaptureSession::frameReady, this, [this, adapterIndex, outputIndex](QImage frame) {
        const QString key = keyFor(adapterIndex, outputIndex);
        setMonitorStatus(key, QStringLiteral("Live"));
        emit frameReady(adapterIndex, outputIndex, std::move(frame));
    });
    connect(session, &CaptureSession::captureError, this, [this, adapterIndex, outputIndex](const QString& message) {
        const QString key = keyFor(adapterIndex, outputIndex);
        m_blockedErrorKeys.insert(key);
        setMonitorStatus(key, QStringLiteral("Error: %1").arg(message));
        stopSession(key, false);
        reconcile();
    });

    session->startCapture();
}

void CaptureController::stopSession(const QString& key, bool setIdleStatus) {
    auto it = m_sessions.find(key);
    if (it == m_sessions.end()) return;

    ActiveSession active = it.value();
    m_sessions.erase(it);
    if (active.session) {
        active.session->stopCapture();
        active.session->deleteLater();
    }
    if (setIdleStatus) setMonitorStatus(key, QStringLiteral("Idle"));
    emit frameCleared(active.adapterIndex, active.outputIndex);
}

void CaptureController::startWindowSession(quintptr hwnd) {
    const QString key = keyForWindow(hwnd);
    CaptureSession* session = m_windowFactory(hwnd, this);
    ActiveWindowSession active{hwnd, session};
    m_windowSessions.insert(key, active);

    connect(session, &CaptureSession::frameReady, this, [this, hwnd](QImage frame) {
        emit windowFrameReady(hwnd, std::move(frame));
    });
    connect(session, &CaptureSession::captureError, this, [this, hwnd](const QString& /*msg*/) {
        // Window closed or error — stop the session; reconcile will clean up.
        const QString k = keyForWindow(hwnd);
        stopWindowSession(k);
        emit windowFrameCleared(hwnd);
    });

    session->startCapture();
}

void CaptureController::stopWindowSession(const QString& key) {
    auto it = m_windowSessions.find(key);
    if (it == m_windowSessions.end()) return;

    ActiveWindowSession active = it.value();
    m_windowSessions.erase(it);
    if (active.session) {
        active.session->stopCapture();
        active.session->deleteLater();
    }
    emit windowFrameCleared(active.hwnd);
}

void CaptureController::setSummary(const QString& summary) {
    if (m_statusSummary == summary) return;
    m_statusSummary = summary;
    emit statusChanged(summary);
}

void CaptureController::setMonitorStatus(const QString& key, const QString& status) {
    if (m_lastStatus.value(key) == status) return;
    m_lastStatus.insert(key, status);

    const QStringList parts = key.split(QLatin1Char(':'));
    if (parts.size() == 2) {
        emit monitorStatusChanged(parts.at(0).toInt(), parts.at(1).toInt(), status);
    }
}
