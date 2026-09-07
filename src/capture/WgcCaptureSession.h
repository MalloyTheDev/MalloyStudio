#pragma once

#include "CaptureController.h"   // for CaptureSession
#include "WgcCapture.h"

#include <memory>

// CaptureSession implementation backed by a WgcCapture source.
//
// The whole job here is the thread boundary. Frames arrive on a system thread
// and the rest of this application lives on the GUI thread, so each frame is
// posted rather than emitted, and it is posted whole: the frame carries the
// slot it holds in the backend's bounded handoff, so the slot is released when
// the consumer actually has the frame, and released just the same if the
// delivery is discarded because this object went away first.
class WgcCaptureSession final : public CaptureSession {
    Q_OBJECT
public:
    // Captures the monitor behind a DXGI adapter and output index, so a
    // display source names the same screen whichever backend is running.
    WgcCaptureSession(int adapterIndex, int outputIndex, QObject* parent = nullptr);
    // Captures a single window.
    explicit WgcCaptureSession(quintptr hwnd, QObject* parent = nullptr);
    ~WgcCaptureSession() override;

    void startCapture() override;
    void stopCapture() override;
    CaptureStats stats() const override;
    void setDelivering(bool delivering) override;

private:
    void attach();

    int      m_adapterIndex = -1;
    int      m_outputIndex  = -1;
    quintptr m_hwnd         = 0;

    bool m_delivering = true;
    std::unique_ptr<WgcCapture> m_capture;

    // The backend's counters, kept after it is torn down. A run summary is
    // written once the capture has already stopped, and a source's own account
    // of what it produced must not vanish with it.
    CaptureStats m_retired;
};
