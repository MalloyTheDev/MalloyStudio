#include "PreviewWidget.h"
#include "model/Canvas.h"
#include "model/FilterEffect.h"
// ScrollFilter::advance() is called from drawItem; the full definition is in FilterEffect.h
#include "model/Scene.h"
#include "model/SceneCollection.h"
#include "model/SceneItem.h"
#include "model/Source.h"

#include <QBuffer>
#include <QDateTime>
#include <QEasingCurve>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QMutexLocker>
#include <QPainter>
#include <QPaintEvent>
#include <QTimeLine>
#include <QTimer>

namespace {
constexpr qreal HandleCanvasSize = 20.0;
}

PreviewWidget::PreviewWidget(SceneCollection* scenes, Role role, QWidget* parent)
    : QWidget(parent), m_role(role), m_scenes(scenes)
{
    setAutoFillBackground(true);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    QPalette p = palette();
    p.setColor(QPalette::Window, QColor(30, 30, 30));
    setPalette(p);

    connect(m_scenes, &SceneCollection::itemsChanged, this, qOverload<>(&PreviewWidget::update));
    connect(m_scenes, &SceneCollection::currentChanged, this, [this](int){ update(); });
    connect(m_scenes, &SceneCollection::programChanged, this, [this](int){ update(); });
    connect(m_scenes, &SceneCollection::previewChanged, this, [this](int){ update(); });
    connect(m_scenes, &SceneCollection::itemSelectionChanged, this, [this](int){ update(); });
    connect(m_scenes, &SceneCollection::collectionReset, this, [this]{
        m_imageCache.clear();
        clearFrames();
    });
}

QString PreviewWidget::frameKey(int adapterIndex, int outputIndex) {
    return QStringLiteral("%1:%2").arg(adapterIndex).arg(outputIndex);
}

void PreviewWidget::setSceneName(const QString& name) {
    if (m_sceneName == name) return;
    m_sceneName = name;
    update();
}

void PreviewWidget::updateFrame(int adapterIndex, int outputIndex, QImage frame) {
    QMutexLocker lock(&m_frameMutex);
    m_frames.insert(frameKey(adapterIndex, outputIndex), std::move(frame));
    lock.unlock();
    update();
}

void PreviewWidget::clearFrame(int adapterIndex, int outputIndex) {
    QMutexLocker lock(&m_frameMutex);
    m_frames.remove(frameKey(adapterIndex, outputIndex));
    lock.unlock();
    update();
}

void PreviewWidget::clearFrames() {
    QMutexLocker lock(&m_frameMutex);
    m_frames.clear();
    m_windowFrames.clear();
    lock.unlock();
    update();
}

void PreviewWidget::updateWindowFrame(quintptr hwnd, QImage frame) {
    QMutexLocker lock(&m_frameMutex);
    m_windowFrames.insert(hwnd, std::move(frame));
    lock.unlock();
    update();
}

void PreviewWidget::clearWindowFrame(quintptr hwnd) {
    QMutexLocker lock(&m_frameMutex);
    m_windowFrames.remove(hwnd);
    lock.unlock();
    update();
}

QImage PreviewWidget::renderCurrentScene() {
    const QRect r = canvasRect();
    return r.isEmpty() ? QImage{} : grab(r).toImage();
}

void PreviewWidget::beginFadeTransition(QImage from, int durationMs) {
    cancelTransition();
    if (from.isNull() || durationMs <= 0) return;
    m_transFrom   = std::move(from);
    m_transFactor = 0.0f;
    m_transTimeline = new QTimeLine(durationMs, this);
    m_transTimeline->setEasingCurve(QEasingCurve::InOutSine);
    connect(m_transTimeline, &QTimeLine::valueChanged, this, [this](qreal v) {
        m_transFactor = static_cast<float>(v);
        update();
    });
    connect(m_transTimeline, &QTimeLine::finished, this, &PreviewWidget::cancelTransition);
    m_transTimeline->start();
}

void PreviewWidget::cancelTransition() {
    if (m_transTimeline) {
        m_transTimeline->stop();
        m_transTimeline->deleteLater();
        m_transTimeline = nullptr;
    }
    m_transFrom   = {};
    m_transFactor = 1.0f;
    update();
}

QRect PreviewWidget::canvasRect() const {
    const int margin = 16;
    const int availW = width() - 2 * margin;
    const int availH = height() - 2 * margin;
    if (availW <= 0 || availH <= 0) return {};

    constexpr double ratio = 16.0 / 9.0;
    int w = availW;
    int h = static_cast<int>(w / ratio);
    if (h > availH) {
        h = availH;
        w = static_cast<int>(h * ratio);
    }
    return QRect((width() - w) / 2, (height() - h) / 2, w, h);
}

QPointF PreviewWidget::widgetToCanvas(const QPoint& point) const {
    const QRect rect = canvasRect();
    if (rect.isEmpty()) return {};
    return {
        (point.x() - rect.left()) * MalloyCanvas::Width / rect.width(),
        (point.y() - rect.top()) * MalloyCanvas::Height / rect.height()
    };
}

QRectF PreviewWidget::canvasToWidget(const QRectF& rect) const {
    const QRect canvas = canvasRect();
    if (canvas.isEmpty()) return {};
    return {
        canvas.left() + rect.left() * canvas.width() / MalloyCanvas::Width,
        canvas.top() + rect.top() * canvas.height() / MalloyCanvas::Height,
        rect.width() * canvas.width() / MalloyCanvas::Width,
        rect.height() * canvas.height() / MalloyCanvas::Height
    };
}

int PreviewWidget::itemAtCanvasPoint(const QPointF& point) const {
    Scene* scene = nullptr;
    if (m_scenes) {
        scene = (m_role == Role::Program)
            ? m_scenes->sceneAt(m_scenes->programIndex())
            : m_scenes->currentScene();
    }
    if (!scene) return -1;

    for (int i = 0; i < scene->itemCount(); ++i) {
        SceneItem* item = scene->itemAt(i);
        if (item->isVisible() && item->transform().contains(point)) return i;
    }
    return -1;
}

PreviewWidget::DragMode PreviewWidget::handleAtCanvasPoint(SceneItem* item, const QPointF& point) const {
    if (!item || item->isLocked()) return DragMode::None;

    const QRectF r = item->transform();
    const QRectF tl(r.topLeft() - QPointF(HandleCanvasSize / 2.0, HandleCanvasSize / 2.0),
                    QSizeF(HandleCanvasSize, HandleCanvasSize));
    const QRectF tr(r.topRight() - QPointF(HandleCanvasSize / 2.0, HandleCanvasSize / 2.0),
                    QSizeF(HandleCanvasSize, HandleCanvasSize));
    const QRectF bl(r.bottomLeft() - QPointF(HandleCanvasSize / 2.0, HandleCanvasSize / 2.0),
                    QSizeF(HandleCanvasSize, HandleCanvasSize));
    const QRectF br(r.bottomRight() - QPointF(HandleCanvasSize / 2.0, HandleCanvasSize / 2.0),
                    QSizeF(HandleCanvasSize, HandleCanvasSize));

    if (tl.contains(point)) return DragMode::ResizeTopLeft;
    if (tr.contains(point)) return DragMode::ResizeTopRight;
    if (bl.contains(point)) return DragMode::ResizeBottomLeft;
    if (br.contains(point)) return DragMode::ResizeBottomRight;
    return DragMode::Move;
}

void PreviewWidget::paintEvent(QPaintEvent*) {
    const QRect canvas = canvasRect();
    // Program role always renders the on-air scene; Staged renders the editable/staged scene.
    Scene* scene = nullptr;
    if (m_scenes) {
        scene = (m_role == Role::Program)
            ? m_scenes->sceneAt(m_scenes->programIndex())
            : m_scenes->sceneAt(m_scenes->currentIndex());
    }

    // --- 1. Render the scene off-screen at canvas-native resolution.
    //        This is what the Recorder consumes (always 1920×1080 regardless
    //        of how the user sized the preview dock), and it also avoids the
    //        recursion that calling grab() from paintEvent would cause.
    QImage composed(MalloyCanvas::Width, MalloyCanvas::Height, QImage::Format_ARGB32_Premultiplied);
    composed.fill(QColor(0, 0, 0));
    if (scene) {
        QPainter p(&composed);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.setRenderHint(QPainter::Antialiasing);
        for (int i = scene->itemCount() - 1; i >= 0; --i) {
            SceneItem* item = scene->itemAt(i);
            if (!item->isVisible()) continue;
            Source* source = m_scenes->sourceForItem(item);
            drawItem(p, item, source, item->transform(), /*selected=*/false);
        }
        // Transition overlay in canvas coords so it's also captured by the recorder
        if (!m_transFrom.isNull() && m_transFactor < 1.0f) {
            p.setOpacity(1.0 - static_cast<double>(m_transFactor));
            p.drawImage(composed.rect(), m_transFrom);
            p.setOpacity(1.0);
        }
    }

    // --- 2. Publish to the cache for Recorder (Program role only).
    if (m_role == Role::Program) {
        QMutexLocker lock(&m_composedMutex);
        m_composedFrame = composed;
    }

    // --- 3. Blit composed image to widget; selection handles drawn last
    //        (in widget pixel coords, not captured).
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setRenderHint(QPainter::Antialiasing);
    if (canvas.isEmpty()) return;

    // Border colour: red if Program in studio mode, grey otherwise.
    const bool inStudio = m_scenes && m_scenes->studioMode();
    const bool isProgramMonitor = (m_role == Role::Program) && inStudio;
    painter.setPen(QPen(isProgramMonitor ? QColor(220, 50, 50) : QColor(78, 78, 78), isProgramMonitor ? 2 : 1));
    painter.drawRect(canvas);
    if (scene) {
        painter.drawImage(canvas, composed);
        // Show selection handles only when editing is meaningful:
        // - Staged role: always (user edits staged scene)
        // - Program role: only when not in studio mode (single-canvas workflow)
        if (m_role == Role::Staged || !inStudio) {
            for (int i = scene->itemCount() - 1; i >= 0; --i) {
                SceneItem* item = scene->itemAt(i);
                if (!item->isVisible() || !item->isSelected()) continue;
                drawSelection(painter, canvasToWidget(item->transform()), item->isLocked());
            }
        }
    } else {
        painter.fillRect(canvas, QColor(0, 0, 0));
        const QString label = m_sceneName.isEmpty()
            ? QStringLiteral("No scene selected")
            : m_sceneName;
        painter.setPen(QColor(180, 180, 180));
        QFont f = font();
        f.setPointSize(14);
        painter.setFont(f);
        painter.drawText(canvas, Qt::AlignCenter, label);
    }

    // Role badge overlay (top-centre of the canvas rect).
    if (inStudio) {
        const QString badge = (m_role == Role::Program)
            ? QStringLiteral("● LIVE")
            : QStringLiteral("◎ PREVIEW");
        const QColor badgeBg = (m_role == Role::Program)
            ? QColor(200, 40, 40, 220)
            : QColor(40, 120, 200, 220);
        QFont bf = font();
        bf.setPixelSize(12);
        bf.setBold(true);
        painter.setFont(bf);
        const QFontMetrics fm(bf);
        const QRect br = fm.boundingRect(badge).adjusted(-6, -2, 6, 2);
        const QPoint origin(canvas.center().x() - br.width() / 2, canvas.top() + 6);
        painter.fillRect(QRect(origin, br.size()), badgeBg);
        painter.setPen(Qt::white);
        painter.drawText(QRect(origin, br.size()), Qt::AlignCenter, badge);
    }
}

QImage PreviewWidget::cachedComposedFrame() const {
    QMutexLocker lock(&m_composedMutex);
    return m_composedFrame; // QImage is implicitly shared; safe copy
}

void PreviewWidget::drawSourceDirect(QPainter& painter, Source* source, const QRectF& rect) {
    // Fetch frames under the mutex.
    QMutexLocker lock(&m_frameMutex);
    const QImage displayFrame = (source->type() == Source::Type::DisplayCapture && source->hasMonitorConfig())
        ? m_frames.value(frameKey(source->adapterIndex(), source->outputIndex()))
        : QImage{};
    const QImage windowFrame = (source->type() == Source::Type::WindowCapture)
        ? m_windowFrames.value(source->windowHandle())
        : QImage{};
    lock.unlock();

    switch (source->type()) {
        case Source::Type::DisplayCapture:
            if (!displayFrame.isNull()) {
                painter.drawImage(rect, displayFrame);
            } else {
                painter.fillRect(rect, QColor(12, 18, 24));
                painter.setPen(QColor(100, 150, 210));
                painter.drawText(rect, Qt::AlignCenter, source->hasMonitorConfig()
                    ? QStringLiteral("Display Capture waiting")
                    : QStringLiteral("Display Capture needs monitor"));
            }
            break;
        case Source::Type::WindowCapture:
            if (!windowFrame.isNull()) {
                painter.drawImage(rect, windowFrame);
            } else {
                painter.fillRect(rect, QColor(14, 20, 28));
                painter.setPen(QColor(120, 165, 220));
                painter.drawText(rect, Qt::AlignCenter, source->hasWindowConfig()
                    ? QStringLiteral("\xF4\xB0\x8D\xBB %1").arg(source->windowTitle())  // 🖻 glyph
                    : QStringLiteral("Window Capture — pick a window in Inspector"));
            }
            break;
        case Source::Type::AudioInput:
            // Audio sources have no visual content; draw a subtle indicator.
            painter.fillRect(rect, QColor(28, 22, 38, 180));
            painter.setPen(QColor(180, 140, 220));
            painter.drawText(rect, Qt::AlignCenter,
                             QStringLiteral("\xF0\x9F\x8E\xA4 %1").arg(source->name()));  // 🎤
            break;
        case Source::Type::Text: {
            painter.fillRect(rect, QColor(0, 0, 0, 80));
            painter.setPen(QColor(245, 245, 245));
            QFont f = font();
            f.setBold(true);
            f.setPointSizeF(qMax(12.0, rect.height() / 4.0));
            painter.setFont(f);
            painter.drawText(rect.adjusted(12, 8, -12, -8), Qt::AlignCenter | Qt::TextWordWrap, source->text());
            break;
        }
        case Source::Type::ColorBlock:
            painter.fillRect(rect, source->color());
            break;
        case Source::Type::Image: {
            const QString& path = source->imagePath();
            if (!path.isEmpty()) {
                if (!m_imageCache.contains(path))
                    m_imageCache.insert(path, QImage(path));
                const QImage& img = m_imageCache[path];
                if (!img.isNull()) {
                    painter.drawImage(rect, img);
                    break;
                }
            }
            painter.fillRect(rect, QColor(36, 42, 52));
            painter.setPen(QColor(170, 180, 200));
            painter.drawText(rect, Qt::AlignCenter, path.isEmpty()
                ? QStringLiteral("Image (no file)")
                : QStringLiteral("Image (load failed)"));
            break;
        }
        case Source::Type::Browser:
            painter.fillRect(rect, QColor(30, 36, 40));
            painter.setPen(QColor(170, 210, 190));
            painter.drawText(rect, Qt::AlignCenter, QStringLiteral("Browser placeholder"));
            break;
    }
}

void PreviewWidget::drawItem(QPainter& painter, SceneItem* item, Source* source, const QRectF& rect, bool selected) {
    if (!source) return;

    const QList<FilterEffect*>& filters = item->filters();

    if (filters.isEmpty()) {
        // Fast path: render directly into the composed image.
        drawSourceDirect(painter, source, rect);
    } else {
        // Slow path: render source into a temporary QImage, apply filters, then blit.
        const QSize imgSize = rect.toRect().size().expandedTo(QSize(1, 1));
        QImage img(imgSize, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        {
            QPainter tmp(&img);
            tmp.setRenderHints(QPainter::SmoothPixmapTransform | QPainter::Antialiasing);
            drawSourceDirect(tmp, source, QRectF(QPointF(0.0, 0.0), rect.size()));
        }

        // Apply non-opacity filters in order (modifies img in-place).
        const qint64 nowUs = QDateTime::currentMSecsSinceEpoch() * 1000;
        for (FilterEffect* f : filters) {
            if (f->type() == FilterEffect::Type::Opacity) continue;
            if (f->type() == FilterEffect::Type::Scroll)
                static_cast<const ScrollFilter*>(f)->advance(nowUs);
            f->apply(img);
        }

        // Accumulate opacity from all OpacityFilters.
        double opacity = 1.0;
        for (const FilterEffect* f : filters) {
            if (f->type() == FilterEffect::Type::Opacity)
                opacity *= static_cast<const OpacityFilter*>(f)->opacity();
        }

        painter.setOpacity(opacity);
        painter.drawImage(rect, img);
        painter.setOpacity(1.0);
    }

    if (selected) drawSelection(painter, rect, item->isLocked());
}

void PreviewWidget::drawSelection(QPainter& painter, const QRectF& rect, bool locked) {
    const QColor color = locked ? QColor(230, 150, 80) : QColor(42, 170, 255);
    painter.setPen(QPen(color, 2));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(rect);

    if (locked) return;

    painter.setBrush(color);
    painter.setPen(QPen(QColor(20, 20, 20), 1));
    const QSizeF s(8, 8);
    const QPointF points[] = {rect.topLeft(), rect.topRight(), rect.bottomLeft(), rect.bottomRight()};
    for (const QPointF& p : points) {
        painter.drawRect(QRectF(p.x() - s.width() / 2.0, p.y() - s.height() / 2.0, s.width(), s.height()));
    }
}

void PreviewWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    // Program widget is read-only in studio mode (you edit via the Staged pane).
    if (m_role == Role::Program && m_scenes && m_scenes->studioMode()) {
        event->ignore();
        return;
    }
    setFocus();

    const QPointF canvasPoint = widgetToCanvas(event->pos());
    const int index = itemAtCanvasPoint(canvasPoint);
    m_scenes->selectCurrentItemAt(index);

    SceneItem* item = m_scenes->currentScene() ? m_scenes->currentScene()->itemAt(index) : nullptr;
    if (!item || item->isLocked()) {
        m_dragMode = DragMode::None;
        return;
    }

    m_dragIndex = index;
    m_dragStartCanvas = canvasPoint;
    m_dragStartRect = item->transform();
    m_scenes->beginCurrentItemTransformEdit(index);
    m_dragMode = handleAtCanvasPoint(item, canvasPoint);
}

void PreviewWidget::mouseMoveEvent(QMouseEvent* event) {
    if (m_dragMode == DragMode::None || m_dragIndex < 0) return;
    SceneItem* item = m_scenes->currentScene() ? m_scenes->currentScene()->itemAt(m_dragIndex) : nullptr;
    if (!item || item->isLocked()) return;

    const QPointF canvasPoint = widgetToCanvas(event->pos());
    if (m_dragMode == DragMode::Move) {
        const QPointF delta = canvasPoint - m_dragStartCanvas;
        m_scenes->updateCurrentItemTransformEdit(m_dragIndex, MalloyCanvas::snapRect(m_dragStartRect.translated(delta)));
    } else {
        m_scenes->updateCurrentItemTransformEdit(m_dragIndex, resizedRect(canvasPoint));
    }
}

void PreviewWidget::mouseReleaseEvent(QMouseEvent*) {
    if (m_dragIndex >= 0) m_scenes->commitCurrentItemTransformEdit();
    m_dragMode = DragMode::None;
    m_dragIndex = -1;
}

void PreviewWidget::keyPressEvent(QKeyEvent* event) {
    if (m_role == Role::Program && m_scenes && m_scenes->studioMode()) {
        QWidget::keyPressEvent(event);
        return;
    }
    const int index = m_scenes->currentItemIndex();
    SceneItem* item = m_scenes->currentItem();
    if (!item) {
        QWidget::keyPressEvent(event);
        return;
    }

    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        m_scenes->removeCurrentItemAt(index);
        return;
    }

    if (item->isLocked()) {
        QWidget::keyPressEvent(event);
        return;
    }

    const qreal step = event->modifiers() & Qt::ShiftModifier ? 10.0 : 1.0;
    switch (event->key()) {
        case Qt::Key_Left:  m_scenes->moveCurrentItemBy(index, -step, 0.0); return;
        case Qt::Key_Right: m_scenes->moveCurrentItemBy(index, step, 0.0); return;
        case Qt::Key_Up:    m_scenes->moveCurrentItemBy(index, 0.0, -step); return;
        case Qt::Key_Down:  m_scenes->moveCurrentItemBy(index, 0.0, step); return;
        default:
            QWidget::keyPressEvent(event);
    }
}

QRectF PreviewWidget::resizedRect(const QPointF& canvasPoint) const {
    QRectF next = m_dragStartRect;
    switch (m_dragMode) {
        case DragMode::ResizeTopLeft:
            next.setTopLeft(canvasPoint);
            break;
        case DragMode::ResizeTopRight:
            next.setTopRight(canvasPoint);
            break;
        case DragMode::ResizeBottomLeft:
            next.setBottomLeft(canvasPoint);
            break;
        case DragMode::ResizeBottomRight:
            next.setBottomRight(canvasPoint);
            break;
        default:
            break;
    }
    return MalloyCanvas::clampRect(next.normalized());
}

// ── Replay buffer ──────────────────────────────────────────────────────────

void PreviewWidget::setReplayBufferSeconds(int seconds) {
    if (m_role != Role::Program) return;   // only Program pane captures replay
    m_replaySeconds = seconds;
    if (seconds > 0) {
        if (!m_replayTimer) {
            m_replayTimer = new QTimer(this);
            m_replayTimer->setInterval(200);   // 5 fps — keeps ring at ~22 MB for 30 s
            connect(m_replayTimer, &QTimer::timeout, this, &PreviewWidget::captureReplayFrame);
        }
        m_replayTimer->start();
    } else {
        if (m_replayTimer) m_replayTimer->stop();
        QMutexLocker lock(&m_replayMutex);
        m_replayFrames.clear();
    }
}

void PreviewWidget::captureReplayFrame() {
    if (m_replaySeconds <= 0) return;

    const QImage frame = cachedComposedFrame();
    if (frame.isNull()) return;

    // JPEG-compress to keep ring memory usage manageable.
    QByteArray data;
    QBuffer buf(&data);
    buf.open(QIODevice::WriteOnly);
    frame.save(&buf, "JPEG", 75);
    buf.close();

    const qint64 nowUs = QDateTime::currentMSecsSinceEpoch() * 1000;
    const qint64 maxSpanUs = static_cast<qint64>(m_replaySeconds) * 1'000'000;

    QMutexLocker lock(&m_replayMutex);
    m_replayFrames.enqueue({std::move(data), nowUs});
    // Trim frames outside the window.
    while (m_replayFrames.size() > 1) {
        const qint64 span = m_replayFrames.back().ptsUs - m_replayFrames.front().ptsUs;
        if (span <= maxSpanUs) break;
        m_replayFrames.dequeue();
    }
}

QQueue<ReplayFrame> PreviewWidget::snapshotReplayFrames() const {
    QMutexLocker lock(&m_replayMutex);
    return m_replayFrames;   // QQueue copy (implicit sharing of QByteArray blobs)
}
