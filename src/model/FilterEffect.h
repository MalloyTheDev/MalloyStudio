#pragma once
#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QString>

// ---------------------------------------------------------------------------
// FilterEffect — base class for per-layer visual filters.
//
// Filters are stored on SceneItem (QObject-parented) and applied by
// PreviewWidget::drawItem() in chain order after the source renders into a
// temporary QImage. OpacityFilter is special-cased: its apply() is a no-op;
// the painter opacity is set from the accumulated filter values instead to
// avoid a full pixel-loop.
// ---------------------------------------------------------------------------
class FilterEffect : public QObject {
    Q_OBJECT
public:
    enum class Type { Crop, Opacity, ColorCorrection, ChromaKey, Blur, Scroll };

    explicit FilterEffect(QObject* parent = nullptr) : QObject(parent) {}

    virtual Type          type()   const = 0;
    virtual QString       label()  const = 0;

    // Modifies img in-place. Called only for non-Opacity filters.
    virtual void          apply(QImage& img) const = 0;

    virtual QJsonObject   toJson() const = 0;

    // Deep-copy. The caller takes ownership (or passes a parent).
    virtual FilterEffect* clone(QObject* parent = nullptr) const = 0;

    // Factory: creates the right concrete type from a JSON object.
    static FilterEffect* fromJson(const QJsonObject& obj, QObject* parent = nullptr);

    // v7 Tier 3: per-filter enable flag. PreviewWidget::drawItem() skips
    // disabled filters in the chain (and OpacityFilter's accumulator excludes
    // disabled instances). JSON round-trips via `"enabled": <bool>`; missing
    // key defaults to true so older project files load with everything on.
    bool isEnabled() const { return m_enabled; }
    void setEnabled(bool e);

signals:
    void changed();

protected:
    // Concrete subclasses' toJson() should call writeBaseFields() to emit
    // `enabled` alongside their type-specific keys, keeping the schema uniform.
    QJsonObject writeBaseFields(QJsonObject obj) const;
    // Mirror of writeBaseFields for the factory side — fromJson() reads
    // shared keys back into the freshly-constructed filter.
    void readBaseFields(const QJsonObject& obj);

private:
    bool m_enabled = true;
};

// ---------------------------------------------------------------------------
// CropFilter — removes a fraction of each edge and scales back to fill rect.
// All values are 0..1 fractions of the item's width/height.
// ---------------------------------------------------------------------------
class CropFilter final : public FilterEffect {
    Q_OBJECT
public:
    explicit CropFilter(QObject* parent = nullptr);

    float top()    const { return m_top; }
    float left()   const { return m_left; }
    float bottom() const { return m_bottom; }
    float right()  const { return m_right; }

    void setTop(float v);
    void setLeft(float v);
    void setBottom(float v);
    void setRight(float v);

    Type          type()  const override { return Type::Crop; }
    QString       label() const override { return QStringLiteral("Crop"); }
    void          apply(QImage& img) const override;
    QJsonObject   toJson() const override;
    FilterEffect* clone(QObject* parent = nullptr) const override;

private:
    float m_top    = 0.0f;
    float m_left   = 0.0f;
    float m_bottom = 0.0f;
    float m_right  = 0.0f;
};

// ---------------------------------------------------------------------------
// OpacityFilter — 0..1 opacity. apply() is a no-op; PreviewWidget accumulates
// opacity() values and calls painter.setOpacity() before drawing the img.
// ---------------------------------------------------------------------------
class OpacityFilter final : public FilterEffect {
    Q_OBJECT
public:
    explicit OpacityFilter(QObject* parent = nullptr);

    float opacity() const { return m_opacity; }
    void  setOpacity(float v);

    Type          type()  const override { return Type::Opacity; }
    QString       label() const override { return QStringLiteral("Opacity"); }
    void          apply(QImage&) const override {}   // handled via painter.setOpacity
    QJsonObject   toJson() const override;
    FilterEffect* clone(QObject* parent = nullptr) const override;

private:
    float m_opacity = 1.0f;
};

// ---------------------------------------------------------------------------
// ColorCorrectionFilter — per-channel brightness / contrast / saturation.
// All values are 0..2 floats; 1.0 = neutral (no change).
// Applied via an 8-bit LUT + luma-weighted saturation pixel loop.
// Performance note: applied at the item's rect size, not the full canvas.
// ---------------------------------------------------------------------------
class ColorCorrectionFilter final : public FilterEffect {
    Q_OBJECT
public:
    explicit ColorCorrectionFilter(QObject* parent = nullptr);

    float brightness() const { return m_brightness; }
    float contrast()   const { return m_contrast; }
    float saturation() const { return m_saturation; }

    void setBrightness(float v);
    void setContrast(float v);
    void setSaturation(float v);

    Type          type()  const override { return Type::ColorCorrection; }
    QString       label() const override { return QStringLiteral("Color Correction"); }
    void          apply(QImage& img) const override;
    QJsonObject   toJson() const override;
    FilterEffect* clone(QObject* parent = nullptr) const override;

private:
    float m_brightness = 1.0f;
    float m_contrast   = 1.0f;
    float m_saturation = 1.0f;
};

// ---------------------------------------------------------------------------
// ChromaKeyFilter — removes a specific color from the source using an
// RGB-Euclidean distance metric with a smoothstep falloff.
// ---------------------------------------------------------------------------
class ChromaKeyFilter final : public FilterEffect {
    Q_OBJECT
public:
    explicit ChromaKeyFilter(QObject* parent = nullptr);

    QColor key()        const { return m_key; }
    float  tolerance()  const { return m_tolerance; }   // 0..1
    float  smoothness() const { return m_smoothness; }  // 0..1

    void setKey(const QColor& c);
    void setTolerance(float v);
    void setSmoothness(float v);

    Type          type()  const override { return Type::ChromaKey; }
    QString       label() const override { return QStringLiteral("Chroma Key"); }
    void          apply(QImage& img) const override;
    QJsonObject   toJson() const override;
    FilterEffect* clone(QObject* parent = nullptr) const override;

private:
    QColor m_key        = QColor(0, 255, 0);
    float  m_tolerance  = 0.20f;
    float  m_smoothness = 0.10f;
};

// ---------------------------------------------------------------------------
// BlurFilter — separable box blur with O(W+H) per row/column (sliding window).
// ---------------------------------------------------------------------------
class BlurFilter final : public FilterEffect {
    Q_OBJECT
public:
    explicit BlurFilter(QObject* parent = nullptr);

    int radius() const { return m_radius; }   // 0..32 pixels
    void setRadius(int r);

    Type          type()  const override { return Type::Blur; }
    QString       label() const override { return QStringLiteral("Blur"); }
    void          apply(QImage& img) const override;
    QJsonObject   toJson() const override;
    FilterEffect* clone(QObject* parent = nullptr) const override;

private:
    int m_radius = 4;
};

// ---------------------------------------------------------------------------
// ScrollFilter — translates the source image with seamless wrap at a
// configurable speed (pixels per second). Rendering state (offset, last tick)
// is mutable so advance() can be called from a const context.
//
// PreviewWidget::drawItem() calls advance() before apply() each frame.
// ---------------------------------------------------------------------------
class ScrollFilter final : public FilterEffect {
    Q_OBJECT
public:
    explicit ScrollFilter(QObject* parent = nullptr);

    float speedX() const { return m_speedX; }   // px/s, positive = scroll left
    float speedY() const { return m_speedY; }   // px/s, positive = scroll up
    void  setSpeedX(float v);
    void  setSpeedY(float v);

    // Advance the internal offset by the elapsed time since the last call.
    // Must be called before apply() each frame. Thread-safe against simultaneous
    // read from a different thread only if the caller serialises with apply().
    void advance(qint64 nowUs) const;

    Type          type()  const override { return Type::Scroll; }
    QString       label() const override { return QStringLiteral("Scroll"); }
    void          apply(QImage& img) const override;
    QJsonObject   toJson() const override;
    FilterEffect* clone(QObject* parent = nullptr) const override;

private:
    float m_speedX = 0.0f;
    float m_speedY = 0.0f;

    mutable float   m_offsetX    = 0.0f;
    mutable float   m_offsetY    = 0.0f;
    mutable qint64  m_lastTickUs = 0;
};
