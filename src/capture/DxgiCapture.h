#pragma once
#include <QThread>
#include <QImage>
#include <atomic>

// Captures a single monitor using DXGI Desktop Duplication.
// Runs the capture loop on its own thread; emits frameReady on each new frame.
// All DXGI/D3D11 objects live entirely on the worker thread.
class DxgiCapture : public QThread {
    Q_OBJECT
public:
    explicit DxgiCapture(int adapterIndex, int outputIndex, QObject* parent = nullptr);
    ~DxgiCapture() override;

    void requestStop();

signals:
    void frameReady(QImage frame);
    void captureError(QString message);

protected:
    void run() override;

private:
    int m_adapterIndex;
    int m_outputIndex;
    std::atomic<bool> m_running{false};
};
