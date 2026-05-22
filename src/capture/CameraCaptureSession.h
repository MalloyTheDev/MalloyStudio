#pragma once
#include "CaptureController.h"   // for CaptureSession base
#include "CameraCapture.h"

#include <QString>

// CaptureSession implementation backed by a CameraCapture worker thread.
class CameraCaptureSession final : public CaptureSession {
    Q_OBJECT
public:
    explicit CameraCaptureSession(const QString& deviceId, QObject* parent = nullptr);
    ~CameraCaptureSession() override;

    void startCapture() override;
    void stopCapture() override;

private:
    QString        m_deviceId;
    CameraCapture* m_capture = nullptr;
};
