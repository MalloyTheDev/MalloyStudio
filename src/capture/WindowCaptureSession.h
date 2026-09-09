#pragma once
#include "CaptureController.h"   // for CaptureSession base
#include "WindowCapture.h"

// CaptureSession implementation backed by a WindowCapture worker thread.
class WindowCaptureSession final : public CaptureSession {
    Q_OBJECT
public:
    explicit WindowCaptureSession(quintptr hwnd, QObject* parent = nullptr);
    ~WindowCaptureSession() override;

    void startCapture() override;
    void stopCapture() override;
    CaptureStats stats() const override;
    void setDelivering(bool delivering) override;

private:
    quintptr       m_hwnd;
    WindowCapture* m_capture = nullptr;
    CaptureStats   m_retired;
    bool           m_delivering = true;
    quint64        m_generation = 0;
};
