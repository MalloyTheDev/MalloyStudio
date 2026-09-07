#pragma once

#include "ICaptureSource.h"

#include <QString>
#include <QtGlobal>

#include <chrono>
#include <memory>

// ---------------------------------------------------------------------------
// WgcCapture: a capture backend built on Windows.Graphics.Capture.
//
// The difference from the DXGI Desktop Duplication backend is who decides when
// a frame exists. Duplication is asked: a loop calls AcquireNextFrame with a
// timeout and finds out whether anything was presented. WGC announces: the
// system invokes FrameArrived when it has a frame, and nothing here polls.
// That is the point of this backend, and the reason the abstraction it
// implements puts arrival in a callback.
//
// It can capture a monitor or a single window. Window capture here is not the
// same mechanism as the PrintWindow path in WindowCapture: this one is
// composited by the system, so it survives occlusion and does not depend on an
// application cooperating with PW_RENDERFULLCONTENT.
//
// Deliberately still a CPU readback. The frame arrives as a D3D11 texture and
// is copied down to a QImage before it is handed on, exactly as the DXGI
// backend does, so that comparing the two backends compares acquisition and
// nothing else. Keeping frames on the GPU is worth doing and is a separate
// change; doing both at once would make a large improvement uninterpretable.
// ---------------------------------------------------------------------------
class WgcCapture final : public ICaptureSource {
public:
    // Whether this machine can capture through WGC at all: the runtime classes
    // must be present (Windows 10 1903 or later for the free threaded frame
    // pool) and GraphicsCaptureSession::IsSupported must agree.
    //
    // Cheap to call and safe before any capture exists, so callers can fall
    // back to the DXGI backend rather than failing a recording.
    static bool isAvailable();

    // The monitor behind a DXGI adapter and output index, as an HMONITOR, or
    // nullptr if that pair names no attached output.
    //
    // WGC identifies a display by HMONITOR while the rest of this application
    // identifies one by the DXGI pair, and a comparison between the two
    // backends is only meaningful if both are pointed at the same screen. This
    // is that translation, and it is the only place it happens.
    static void* monitorHandleFor(int adapterIndex, int outputIndex);

    // Captures the given monitor (an HMONITOR, as returned above).
    explicit WgcCapture(void* monitorHandle);
    // Captures the given window.
    explicit WgcCapture(quintptr hwnd);
    ~WgcCapture() override;

    WgcCapture(const WgcCapture&) = delete;
    WgcCapture& operator=(const WgcCapture&) = delete;

    // Reported once, from the delivery thread, when capture cannot continue:
    // a lost device, a frame pool that will not recreate, an activation that
    // failed. Set before start().
    using ErrorHandler = std::function<void(QString)>;
    void setErrorHandler(ErrorHandler handler);

    // Reported when the captured item goes away underneath the session: the
    // window closed, the monitor was disconnected. Distinct from an error,
    // because nothing went wrong. Set before start().
    //
    // Unlike frame arrival, this one is delivered through the apartment that
    // started the capture rather than on a system thread, and it was measured
    // arriving only once that thread pumped messages. The application always
    // does, since it starts capture from the GUI thread; a future caller that
    // starts one from a bare worker thread would wait for this forever, and
    // would need a message loop of its own.
    using ClosedHandler = std::function<void()>;
    void setClosedHandler(ClosedHandler handler);

    // ICaptureSource
    bool start(FrameCallback onFrame) override;
    void stop() override;
    CaptureStats stats() const override;

    // How many times the frame pool was rebuilt because the captured content
    // changed size. Not an error count: a window being resized while it is
    // captured is ordinary, and each rebuild is a deliberate state transition
    // rather than a caught failure. Reported at the end of a run so a
    // measurement taken across a resize can be recognised as one.
    int poolRecreations() const;

    // What the last start() failed for, when it returned false.
    QString lastError() const;

    // Converts a WGC SystemRelativeTime, in 100 ns units, to the steady clock
    // this application times frames on.
    //
    // Both count from boot on the same performance counter, so the conversion
    // is a unit change and not an estimate. It is still checked rather than
    // assumed: if the result lands more than a few seconds away from now, the
    // premise is wrong on this machine and the arrival time is the honest
    // answer instead of a confidently wrong one. Pure and passed its own idea
    // of "now" so the rule can be tested without a capture session.
    //
    // Returns true when the backend stamp was used, false when it was
    // rejected and now was substituted.
    static bool normaliseBackendTime(qint64 systemRelativeTime100ns,
                                     std::chrono::steady_clock::time_point now,
                                     std::chrono::steady_clock::time_point* out);

    // Every WinRT type this backend touches lives in here, so that a header
    // including this one does not also include windows.h and the capture ABI.
    // Named rather than anonymous because the event sinks are free functions
    // of the platform's making and have to be able to talk to it.
    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};
