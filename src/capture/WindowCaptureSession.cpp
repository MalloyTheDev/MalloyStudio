#include "WindowCaptureSession.h"

WindowCaptureSession::WindowCaptureSession(quintptr hwnd, QObject* parent)
    : CaptureSession(parent), m_hwnd(hwnd) {}

WindowCaptureSession::~WindowCaptureSession() {
    stopCapture();
}

void WindowCaptureSession::startCapture() {
    if (m_capture) return;
    m_capture = new WindowCapture(m_hwnd, this);
    const quint64 generation = ++m_generation;
    connect(m_capture, &WindowCapture::frameReady,
            this, [this, generation](QImage frame) {
                if (generation == m_generation) emit frameReady(std::move(frame));
            }, Qt::QueuedConnection);
    connect(m_capture, &WindowCapture::captureError,
            this,      &CaptureSession::captureError);
    // windowClosed is forwarded as an error so CaptureController can reconcile.
    connect(m_capture, &WindowCapture::windowClosed, this, [this] {
        emit captureError(QStringLiteral("Window closed"));
    });
    m_capture->setDelivering(m_delivering);
    m_capture->start();
}

void WindowCaptureSession::stopCapture() {
    if (!m_capture) return;
    ++m_generation;
    disconnect(m_capture, nullptr, this, nullptr);
    m_capture->requestStop();
    m_capture->wait(4000);
    m_retired = m_capture->stats();
    delete m_capture;
    m_capture = nullptr;
}

CaptureStats WindowCaptureSession::stats() const {
    return m_capture ? m_capture->stats() : m_retired;
}

void WindowCaptureSession::setDelivering(bool delivering) {
    m_delivering = delivering;
    if (m_capture) m_capture->setDelivering(delivering);
}
