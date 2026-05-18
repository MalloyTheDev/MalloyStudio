#include "WindowCaptureSession.h"

WindowCaptureSession::WindowCaptureSession(quintptr hwnd, QObject* parent)
    : CaptureSession(parent), m_hwnd(hwnd) {}

WindowCaptureSession::~WindowCaptureSession() {
    stopCapture();
}

void WindowCaptureSession::startCapture() {
    if (m_capture) return;
    m_capture = new WindowCapture(m_hwnd, this);
    connect(m_capture, &WindowCapture::frameReady,
            this,      &CaptureSession::frameReady);
    connect(m_capture, &WindowCapture::captureError,
            this,      &CaptureSession::captureError);
    // windowClosed is forwarded as an error so CaptureController can reconcile.
    connect(m_capture, &WindowCapture::windowClosed, this, [this] {
        emit captureError(QStringLiteral("Window closed"));
    });
    m_capture->start();
}

void WindowCaptureSession::stopCapture() {
    if (!m_capture) return;
    m_capture->requestStop();
    m_capture->wait(4000);
    delete m_capture;
    m_capture = nullptr;
}
