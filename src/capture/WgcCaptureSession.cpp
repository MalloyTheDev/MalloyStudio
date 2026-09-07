#include "WgcCaptureSession.h"
#include "platform/FrameProfile.h"

#include <chrono>

#include <QMetaObject>

#include <utility>

WgcCaptureSession::WgcCaptureSession(int adapterIndex, int outputIndex, QObject* parent)
    : CaptureSession(parent), m_adapterIndex(adapterIndex), m_outputIndex(outputIndex) {}

WgcCaptureSession::WgcCaptureSession(quintptr hwnd, QObject* parent)
    : CaptureSession(parent), m_hwnd(hwnd) {}

WgcCaptureSession::~WgcCaptureSession() {
    stopCapture();
}

void WgcCaptureSession::startCapture() {
    if (m_capture) return;

    if (m_hwnd != 0) {
        m_capture = std::make_unique<WgcCapture>(m_hwnd);
    } else {
        void* monitor = WgcCapture::monitorHandleFor(m_adapterIndex, m_outputIndex);
        if (!monitor) {
            emit captureError(QStringLiteral("Monitor not found"));
            return;
        }
        m_capture = std::make_unique<WgcCapture>(monitor);
    }

    attach();

    if (!m_capture->start([this](CapturedFrame&& frame) {
            // On the backend's delivery thread. Nothing is done here beyond
            // handing the frame across: the whole frame is moved into the
            // posted call so that its slot in the bounded handoff lives
            // exactly as long as the delivery does.
            //
            // The stamp measures how long the frame then waits for the GUI
            // thread to come and get it, which is congestion on that thread
            // rather than any property of capture.
            const auto postedAt = std::chrono::steady_clock::now();
            QMetaObject::invokeMethod(
                this,
                [this, postedAt, delivered = std::move(frame)]() mutable {
                    if (FrameProfile::enabled()) {
                        FrameProfile::record(
                            FrameProfile::Stage::HandoffToGui,
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - postedAt).count());
                    }
                    emit frameReady(delivered.image);
                },
                Qt::QueuedConnection);
        })) {
        const QString reason = m_capture->lastError();
        m_retired = m_capture->stats();
        m_capture.reset();
        emit captureError(reason.isEmpty() ? QStringLiteral("Capture could not start") : reason);
    }
}

void WgcCaptureSession::attach() {
    m_capture->setErrorHandler([this](QString message) {
        QMetaObject::invokeMethod(
            this, [this, message] { emit captureError(message); }, Qt::QueuedConnection);
    });
    // A window that closes or a monitor that is unplugged is reported the same
    // way a failure is, because the controller's response to both is the same:
    // stop this session and work out what should be running now.
    m_capture->setClosedHandler([this] {
        QMetaObject::invokeMethod(
            this,
            [this] { emit captureError(QStringLiteral("Capture source closed")); },
            Qt::QueuedConnection);
    });
}

void WgcCaptureSession::stopCapture() {
    if (!m_capture) return;
    m_capture->stop();
    // Read the counters out before the backend goes, so what it produced
    // outlives it.
    m_retired = m_capture->stats();
    m_capture.reset();
}

CaptureStats WgcCaptureSession::stats() const {
    return m_capture ? m_capture->stats() : m_retired;
}
