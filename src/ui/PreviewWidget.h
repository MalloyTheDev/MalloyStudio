#pragma once
#include "media/TimedSource.h"
#include "model/Canvas.h"

#include <QHash>
#include <QImage>
#include <QMutex>
#include <QQueue>
#include <QRectF>
#include <QString>
#include <QWidget>

class SceneCollection;
class SceneItem;
class Source;
class QPainter;
class QTimeLine;
class QTimer;

class PreviewWidget : public QWidget, public TimedFrameSource {
    Q_OBJECT
public:
    // Program: renders programIndex(), is the TimedFrameSource, interactive when !studioMode.
    // Staged:  renders currentIndex(), never updates composedFrame, always interactive.
    enum class Role { Program, Staged };

    explicit PreviewWidget(SceneCollection* scenes, Role role = Role::Program,
                           QWidget* parent = nullptr);

    void setSceneName(const QString& name);

    QImage renderCurrentScene();
    void   beginFadeTransition(QImage from, int durationMs);
    void   cancelTransition();

    // Replay buffer: enable/disable and snapshot the ring.
    // Only the Program role widget captures the replay ring.
    void setReplayBufferSeconds(int seconds);   // 0 = disable
    QQueue<ReplayFrame> snapshotReplayFrames() const;  // thread-safe copy

    // Snapshot of the most recent composed canvas (post-transition overlay),
    // updated at the end of every paintEvent. Thread-safe — Recorder pulls
    // this from a 33 ms timer without triggering extra paint cycles.
    QImage cachedComposedFrame() const;

    // TimedFrameSource interface — currentFrame() returns the cached composed
    // frame (same as cachedComposedFrame). Native dimensions are the canvas
    // constants since PreviewWidget always composes at canvas resolution.
    QImage currentFrame() override { return cachedComposedFrame(); }
    int    nativeWidth()  const override { return static_cast<int>(MalloyCanvas::Width); }
    int    nativeHeight() const override { return static_cast<int>(MalloyCanvas::Height); }

public slots:
    void updateFrame(int adapterIndex, int outputIndex, QImage frame);
    void captureReplayFrame();   // invoked by m_replayTimer (5fps)
    void clearFrame(int adapterIndex, int outputIndex);
    void clearFrames();
    void updateWindowFrame(quintptr hwnd, QImage frame);
    void clearWindowFrame(quintptr hwnd);
    void updateCameraFrame(QString deviceId, QImage frame);
    void clearCameraFrame(QString deviceId);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    QSize sizeHint() const override { return {640, 360}; }
    QSize minimumSizeHint() const override { return {320, 180}; }

private:
    enum class DragMode {
        None,
        Move,
        ResizeTopLeft,
        ResizeTopRight,
        ResizeBottomLeft,
        ResizeBottomRight
    };

    QRect canvasRect() const;
    QPointF widgetToCanvas(const QPoint& point) const;
    QRectF canvasToWidget(const QRectF& rect) const;
    int itemAtCanvasPoint(const QPointF& point) const;
    DragMode handleAtCanvasPoint(SceneItem* item, const QPointF& point) const;
    void drawItem(QPainter& painter, SceneItem* item, Source* source, const QRectF& rect, bool selected);
    void drawSourceDirect(QPainter& painter, Source* source, const QRectF& rect);
    void drawSelection(QPainter& painter, const QRectF& rect, bool locked);
    QRectF resizedRect(const QPointF& canvasPoint) const;
    static QString frameKey(int adapterIndex, int outputIndex);

    Role             m_role = Role::Program;
    SceneCollection* m_scenes = nullptr;
    QString m_sceneName;
    QHash<QString, QImage> m_frames;       // Display capture frames (keyed by "adapter:output")
    QHash<quintptr, QImage> m_windowFrames; // Window capture frames (keyed by HWND)
    QHash<QString, QImage> m_cameraFrames;  // Camera frames (keyed by MF device id)
    QMutex m_frameMutex;                   // Guards all frame caches
    DragMode m_dragMode = DragMode::None;
    int m_dragIndex = -1;
    QPointF m_dragStartCanvas;
    QRectF m_dragStartRect;

    // Scene transition
    QImage     m_transFrom;
    float      m_transFactor   = 1.0f;
    QTimeLine* m_transTimeline = nullptr;

    // Image source cache (main-thread only, keyed by file path)
    QHash<QString, QImage> m_imageCache;

    // Cached most-recent composed canvas (post-overlay). Recorder pulls.
    mutable QMutex m_composedMutex;
    QImage         m_composedFrame;

    // Replay buffer (Program role only). JPEG ring capped by time span.
    QTimer*             m_replayTimer   = nullptr;
    int                 m_replaySeconds = 0;
    mutable QMutex      m_replayMutex;
    QQueue<ReplayFrame> m_replayFrames;
};
