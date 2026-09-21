#include "WindowCapture.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {
constexpr int kTargetFps = 30;
constexpr int kFrameMs   = 1000 / kTargetFps;   // 33 ms
// Failed captures in a row before the worker gives up and reports it, about a
// second's worth. A single failure is skipped with the last frame held; a run
// of them is handed to the controller, which tries again later.
constexpr int kMaxConsecutiveFailures = kTargetFps;
} // namespace

bool WindowCapture::windowExists(quintptr hwnd) {
    return hwnd && IsWindow(reinterpret_cast<HWND>(hwnd));
}

WindowCapture::WindowCapture(quintptr hwnd, QObject* parent)
    : QThread(parent), m_hwnd(hwnd) {}

WindowCapture::~WindowCapture() {
    requestStop();
    wait(4000);
}

void WindowCapture::requestStop() {
    m_running.store(false, std::memory_order_relaxed);
}

void WindowCapture::run() {
    HWND hwnd = reinterpret_cast<HWND>(m_hwnd);
    int failures = 0;

    while (m_running.load(std::memory_order_relaxed)) {
        // Only a window that no longer exists ends the capture.
        if (!IsWindow(hwnd)) {
            emit windowClosed();
            break;
        }

        if (!m_delivering.load(std::memory_order_relaxed)) {
            msleep(kFrameMs);
            continue;
        }

        // Hidden or minimised, there is nothing current to copy, but the
        // window can come back, so the last frame is held. A hidden window
        // used to be reported as closed, which dropped the source until the
        // scene was next edited.
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
            msleep(kFrameMs);
            continue;
        }

        // PrintWindow is serviced by the application that owns the window, so
        // asking a hung one blocks this thread until it recovers, and a stop
        // requested meanwhile cannot be honoured. Hold the last frame instead.
        if (IsHungAppWindow(hwnd)) {
            msleep(kFrameMs);
            continue;
        }

        RECT rc{};
        if (!GetClientRect(hwnd, &rc)) {
            msleep(kFrameMs);
            continue;
        }
        const int w = rc.right  - rc.left;
        const int h = rc.bottom - rc.top;

        if (w <= 0 || h <= 0) {
            msleep(kFrameMs);
            continue;
        }

        // Create compatible DC + bitmap. Either can fail when the system is
        // short of GDI resources, which counts as a failed capture.
        HDC wndDC  = GetDC(hwnd);
        if (!wndDC) { msleep(kFrameMs); continue; }
        HDC memDC  = CreateCompatibleDC(wndDC);
        HBITMAP bmp = memDC ? CreateCompatibleBitmap(wndDC, w, h) : nullptr;

        QByteArray bits;
        int lines = 0;
        if (bmp) {
            HBITMAP old = static_cast<HBITMAP>(SelectObject(memDC, bmp));

            // Try PrintWindow first (works for GPU-rendered / DWM-composited windows).
            // PW_RENDERFULLCONTENT (0x2) captures the fully-composited client buffer.
            BOOL ok = PrintWindow(hwnd, memDC, 0x2 /*PW_RENDERFULLCONTENT*/);
            if (!ok) {
                // Fall back to BitBlt (works for GDI apps, may miss GPU surfaces).
                BitBlt(memDC, 0, 0, w, h, wndDC, 0, 0, SRCCOPY);
            }

            // Extract raw pixels to a QByteArray via GetDIBits.
            BITMAPINFOHEADER bmi{};
            bmi.biSize        = sizeof(BITMAPINFOHEADER);
            bmi.biWidth       = w;
            bmi.biHeight      = -h;   // top-down (matches QImage scanline order)
            bmi.biPlanes      = 1;
            bmi.biBitCount    = 32;
            bmi.biCompression = BI_RGB;

            bits = QByteArray(w * h * 4, Qt::Uninitialized);
            lines = GetDIBits(memDC, bmp, 0, static_cast<UINT>(h),
                              bits.data(),
                              reinterpret_cast<BITMAPINFO*>(&bmi),
                              DIB_RGB_COLORS);

            SelectObject(memDC, old);
            DeleteObject(bmp);
        }
        if (memDC) DeleteDC(memDC);
        ReleaseDC(hwnd, wndDC);

        if (lines > 0) {
            failures = 0;
            // GetDIBits gives BGRA (B, G, R, A) in memory. QImage::Format_ARGB32
            // on little-endian expects the same layout (B@0, G@1, R@2, A@3).
            QImage frame(reinterpret_cast<const uchar*>(bits.constData()),
                         w, h, w * 4, QImage::Format_ARGB32);
            CapturedFrame captured;
            if (m_handoff.tryAcquire(captured)) {
                // Reserve before copying; a full queue costs a dropped frame,
                // not another owned image. The lease travels with the signal.
                captured.image = frame.copy();
                emit frameReady(CaptureFrameHandoff::imageForDelivery(std::move(captured)));
            }
        } else if (++failures >= kMaxConsecutiveFailures) {
            // One failure used to end the session, and with it the source,
            // for good: a window resized between the size query and the copy
            // was enough.
            emit captureError(QStringLiteral("WindowCapture: %1 captures in a row failed")
                                  .arg(failures));
            break;
        }

        msleep(kFrameMs);
    }
}
