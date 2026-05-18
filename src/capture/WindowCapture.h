#pragma once
#include <QImage>
#include <QThread>
#include <QString>

#include <atomic>

// ---------------------------------------------------------------------------
// WindowCapture — QThread worker that captures a specific application window
// (identified by HWND stored as quintptr) using PrintWindow/BitBlt and emits
// QImage frames at approximately 30 fps.
//
// Limitations (same as OBS):
//  - Captures the client area only; window chrome/title bar is excluded.
//  - DRM-protected content (Netflix in browser, etc.) renders as a black frame.
//  - PrintWindow(PW_RENDERFULLCONTENT) requires Windows 8.1+.
// ---------------------------------------------------------------------------
class WindowCapture : public QThread {
    Q_OBJECT
public:
    explicit WindowCapture(quintptr hwnd, QObject* parent = nullptr);
    ~WindowCapture() override;

    // Thread-safe: may be called from any thread.
    void requestStop();

signals:
    void frameReady(QImage frame);
    void captureError(QString message);
    // Emitted when IsWindow(hwnd) returns false — the captured app has closed.
    void windowClosed();

protected:
    void run() override;

private:
    quintptr          m_hwnd;
    std::atomic<bool> m_running{false};
};
