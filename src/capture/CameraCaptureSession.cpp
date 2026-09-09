#include "CameraCaptureSession.h"

CameraCaptureSession::CameraCaptureSession(const QString& deviceId, QObject* parent)
    : CaptureSession(parent), m_deviceId(deviceId) {}

CameraCaptureSession::~CameraCaptureSession() { stopCapture(); }

void CameraCaptureSession::startCapture() {
    if (m_capture) return;
    m_capture = new CameraCapture(this);
    const quint64 generation = ++m_generation;
    // Forward the worker's frames/errors out through the CaptureSession contract.
    connect(m_capture, &CameraCapture::frameReady, this,
            [this, generation](QImage frame) {
                if (generation == m_generation) emit frameReady(std::move(frame));
            }, Qt::QueuedConnection);
    connect(m_capture, &CameraCapture::error,      this, &CaptureSession::captureError);
    m_capture->setDelivering(m_delivering);
    m_capture->start(m_deviceId);
}

void CameraCaptureSession::stopCapture() {
    if (!m_capture) return;
    ++m_generation;
    disconnect(m_capture, nullptr, this, nullptr);
    m_capture->stop();          // joins the read thread
    m_retired = m_capture->stats();
    m_capture->deleteLater();
    m_capture = nullptr;
}

CaptureStats CameraCaptureSession::stats() const {
    return m_capture ? m_capture->stats() : m_retired;
}

void CameraCaptureSession::setDelivering(bool delivering) {
    m_delivering = delivering;
    if (m_capture) m_capture->setDelivering(delivering);
}
