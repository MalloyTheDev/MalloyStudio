#include "WindowCapture.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {
constexpr int kTargetFps = 30;
constexpr int kFrameMs   = 1000 / kTargetFps;   // 33 ms
} // namespace

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
    m_running.store(true, std::memory_order_relaxed);

    HWND hwnd = reinterpret_cast<HWND>(m_hwnd);

    while (m_running.load(std::memory_order_relaxed)) {
        // Check the window is still alive each frame.
        if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) {
            emit windowClosed();
            break;
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

        // Create compatible DC + bitmap.
        HDC wndDC  = GetDC(hwnd);
        if (!wndDC) { msleep(kFrameMs); continue; }
        HDC memDC  = CreateCompatibleDC(wndDC);
        HBITMAP bmp = CreateCompatibleBitmap(wndDC, w, h);
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

        QByteArray bits(w * h * 4, Qt::Uninitialized);
        const int lines = GetDIBits(memDC, bmp, 0, static_cast<UINT>(h),
                                    bits.data(),
                                    reinterpret_cast<BITMAPINFO*>(&bmi),
                                    DIB_RGB_COLORS);

        SelectObject(memDC, old);
        DeleteObject(bmp);
        DeleteDC(memDC);
        ReleaseDC(hwnd, wndDC);

        if (lines > 0) {
            // GetDIBits gives BGRA (B, G, R, A) in memory. QImage::Format_ARGB32
            // on little-endian expects the same layout (B@0, G@1, R@2, A@3).
            QImage frame(reinterpret_cast<const uchar*>(bits.constData()),
                         w, h, w * 4, QImage::Format_ARGB32);
            emit frameReady(frame.copy());   // must copy before bits goes out of scope
        } else {
            emit captureError(QStringLiteral("WindowCapture: GetDIBits failed"));
        }

        msleep(kFrameMs);
    }
}
