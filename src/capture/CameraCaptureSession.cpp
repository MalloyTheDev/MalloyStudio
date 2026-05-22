#include "CameraCaptureSession.h"

CameraCaptureSession::CameraCaptureSession(const QString& deviceId, QObject* parent)
    : CaptureSession(parent), m_deviceId(deviceId) {}

CameraCaptureSession::~CameraCaptureSession() { stopCapture(); }

void CameraCaptureSession::startCapture() {
    if (m_capture) return;
    m_capture = new CameraCapture(this);
    // Forward the worker's frames/errors out through the CaptureSession contract.
    connect(m_capture, &CameraCapture::frameReady, this, &CaptureSession::frameReady);
    connect(m_capture, &CameraCapture::error,      this, &CaptureSession::captureError);
    m_capture->start(m_deviceId);
}

void CameraCaptureSession::stopCapture() {
    if (!m_capture) return;
    m_capture->stop();          // joins the read thread
    m_capture->deleteLater();
    m_capture = nullptr;
}
