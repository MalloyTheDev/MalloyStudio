#include "FilterEffect.h"

#include <QJsonArray>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <vector>

// ---------------------------------------------------------------------------
// FilterEffect — factory
// ---------------------------------------------------------------------------

FilterEffect* FilterEffect::fromJson(const QJsonObject& obj, QObject* parent) {
    const QString typeStr = obj.value(QStringLiteral("type")).toString();
    if (typeStr == QStringLiteral("crop")) {
        auto* f = new CropFilter(parent);
        f->setTop(static_cast<float>(obj.value(QStringLiteral("top")).toDouble(0.0)));
        f->setLeft(static_cast<float>(obj.value(QStringLiteral("left")).toDouble(0.0)));
        f->setBottom(static_cast<float>(obj.value(QStringLiteral("bottom")).toDouble(0.0)));
        f->setRight(static_cast<float>(obj.value(QStringLiteral("right")).toDouble(0.0)));
        return f;
    }
    if (typeStr == QStringLiteral("opacity")) {
        auto* f = new OpacityFilter(parent);
        f->setOpacity(static_cast<float>(obj.value(QStringLiteral("opacity")).toDouble(1.0)));
        return f;
    }
    if (typeStr == QStringLiteral("color_correction")) {
        auto* f = new ColorCorrectionFilter(parent);
        f->setBrightness(static_cast<float>(obj.value(QStringLiteral("brightness")).toDouble(1.0)));
        f->setContrast(static_cast<float>(obj.value(QStringLiteral("contrast")).toDouble(1.0)));
        f->setSaturation(static_cast<float>(obj.value(QStringLiteral("saturation")).toDouble(1.0)));
        return f;
    }
    if (typeStr == QStringLiteral("chroma_key")) {
        auto* f = new ChromaKeyFilter(parent);
        f->setKey(QColor(obj.value(QStringLiteral("key")).toString(QStringLiteral("#00ff00"))));
        f->setTolerance(static_cast<float>(obj.value(QStringLiteral("tolerance")).toDouble(0.20)));
        f->setSmoothness(static_cast<float>(obj.value(QStringLiteral("smoothness")).toDouble(0.10)));
        return f;
    }
    if (typeStr == QStringLiteral("blur")) {
        auto* f = new BlurFilter(parent);
        f->setRadius(obj.value(QStringLiteral("radius")).toInt(4));
        return f;
    }
    if (typeStr == QStringLiteral("scroll")) {
        auto* f = new ScrollFilter(parent);
        f->setSpeedX(static_cast<float>(obj.value(QStringLiteral("speedX")).toDouble(0.0)));
        f->setSpeedY(static_cast<float>(obj.value(QStringLiteral("speedY")).toDouble(0.0)));
        return f;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// CropFilter
// ---------------------------------------------------------------------------

CropFilter::CropFilter(QObject* parent) : FilterEffect(parent) {}

static float clamp01(float v) { return std::max(0.0f, std::min(1.0f, v)); }

void CropFilter::setTop(float v)    { v = clamp01(v); if (m_top    == v) return; m_top    = v; emit changed(); }
void CropFilter::setLeft(float v)   { v = clamp01(v); if (m_left   == v) return; m_left   = v; emit changed(); }
void CropFilter::setBottom(float v) { v = clamp01(v); if (m_bottom == v) return; m_bottom = v; emit changed(); }
void CropFilter::setRight(float v)  { v = clamp01(v); if (m_right  == v) return; m_right  = v; emit changed(); }

void CropFilter::apply(QImage& img) const {
    if (img.isNull()) return;
    const int w = img.width();
    const int h = img.height();
    const int x  = static_cast<int>(m_left   * w);
    const int y  = static_cast<int>(m_top    * h);
    const int cw = w - static_cast<int>((m_left + m_right)  * w);
    const int ch = h - static_cast<int>((m_top  + m_bottom) * h);
    if (cw <= 0 || ch <= 0) { img.fill(Qt::transparent); return; }
    // Crop inner region and scale back to fill the original size so the item
    // rect is always fully covered (crop acts like a zoom-in on the source).
    img = img.copy(x, y, cw, ch).scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

QJsonObject CropFilter::toJson() const {
    return {
        {QStringLiteral("type"),   QStringLiteral("crop")},
        {QStringLiteral("top"),    static_cast<double>(m_top)},
        {QStringLiteral("left"),   static_cast<double>(m_left)},
        {QStringLiteral("bottom"), static_cast<double>(m_bottom)},
        {QStringLiteral("right"),  static_cast<double>(m_right)},
    };
}

FilterEffect* CropFilter::clone(QObject* parent) const {
    auto* f = new CropFilter(parent);
    f->m_top    = m_top;
    f->m_left   = m_left;
    f->m_bottom = m_bottom;
    f->m_right  = m_right;
    return f;
}

// ---------------------------------------------------------------------------
// OpacityFilter
// ---------------------------------------------------------------------------

OpacityFilter::OpacityFilter(QObject* parent) : FilterEffect(parent) {}

void OpacityFilter::setOpacity(float v) {
    v = clamp01(v);
    if (m_opacity == v) return;
    m_opacity = v;
    emit changed();
}

QJsonObject OpacityFilter::toJson() const {
    return {
        {QStringLiteral("type"),    QStringLiteral("opacity")},
        {QStringLiteral("opacity"), static_cast<double>(m_opacity)},
    };
}

FilterEffect* OpacityFilter::clone(QObject* parent) const {
    auto* f = new OpacityFilter(parent);
    f->m_opacity = m_opacity;
    return f;
}

// ---------------------------------------------------------------------------
// ColorCorrectionFilter
// ---------------------------------------------------------------------------

ColorCorrectionFilter::ColorCorrectionFilter(QObject* parent) : FilterEffect(parent) {}

static float clamp02(float v) { return std::max(0.0f, std::min(2.0f, v)); }

void ColorCorrectionFilter::setBrightness(float v) { v = clamp02(v); if (m_brightness == v) return; m_brightness = v; emit changed(); }
void ColorCorrectionFilter::setContrast(float v)   { v = clamp02(v); if (m_contrast   == v) return; m_contrast   = v; emit changed(); }
void ColorCorrectionFilter::setSaturation(float v) { v = clamp02(v); if (m_saturation == v) return; m_saturation = v; emit changed(); }

void ColorCorrectionFilter::apply(QImage& img) const {
    if (img.isNull()) return;
    const bool needBC  = (m_brightness != 1.0f || m_contrast != 1.0f);
    const bool needSat = (m_saturation != 1.0f);
    if (!needBC && !needSat) return;

    // Build 8-bit LUT for brightness + contrast applied to each R/G/B channel.
    quint8 lut[256];
    for (int i = 0; i < 256; ++i) {
        float v = i / 255.0f;
        v *= m_brightness;                        // brightness: scale up/down
        v = (v - 0.5f) * m_contrast + 0.5f;      // contrast: scale around midpoint
        lut[i] = static_cast<quint8>(std::max(0, std::min(255, static_cast<int>(v * 255.0f + 0.5f))));
    }

    img = img.convertToFormat(QImage::Format_ARGB32);

    const int width  = img.width();
    const int height = img.height();

    for (int row = 0; row < height; ++row) {
        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(row));
        for (int col = 0; col < width; ++col) {
            const QRgb px = line[col];
            int r = qRed(px);
            int g = qGreen(px);
            int b = qBlue(px);
            const int a = qAlpha(px);

            if (needSat) {
                // Luma-weighted grayscale (Rec. 601 coefficients × 1000)
                const int gray = (299 * r + 587 * g + 114 * b) / 1000;
                r = std::max(0, std::min(255, static_cast<int>(gray + m_saturation * (r - gray))));
                g = std::max(0, std::min(255, static_cast<int>(gray + m_saturation * (g - gray))));
                b = std::max(0, std::min(255, static_cast<int>(gray + m_saturation * (b - gray))));
            }

            if (needBC) {
                r = lut[static_cast<quint8>(r)];
                g = lut[static_cast<quint8>(g)];
                b = lut[static_cast<quint8>(b)];
            }

            line[col] = qRgba(r, g, b, a);
        }
    }
}

QJsonObject ColorCorrectionFilter::toJson() const {
    return {
        {QStringLiteral("type"),       QStringLiteral("color_correction")},
        {QStringLiteral("brightness"), static_cast<double>(m_brightness)},
        {QStringLiteral("contrast"),   static_cast<double>(m_contrast)},
        {QStringLiteral("saturation"), static_cast<double>(m_saturation)},
    };
}

FilterEffect* ColorCorrectionFilter::clone(QObject* parent) const {
    auto* f = new ColorCorrectionFilter(parent);
    f->m_brightness = m_brightness;
    f->m_contrast   = m_contrast;
    f->m_saturation = m_saturation;
    return f;
}

// ---------------------------------------------------------------------------
// ChromaKeyFilter
// ---------------------------------------------------------------------------

// Normalised max RGB Euclidean distance: sqrt(3 * 255^2)
static constexpr float kMaxRGBDist = 441.6729559f;

ChromaKeyFilter::ChromaKeyFilter(QObject* parent) : FilterEffect(parent) {}

void ChromaKeyFilter::setKey(const QColor& c)   { if (m_key        == c)  return; m_key        = c;  emit changed(); }
void ChromaKeyFilter::setTolerance(float v)  { v = clamp01(v); if (m_tolerance  == v) return; m_tolerance  = v; emit changed(); }
void ChromaKeyFilter::setSmoothness(float v) { v = clamp01(v); if (m_smoothness == v) return; m_smoothness = v; emit changed(); }

void ChromaKeyFilter::apply(QImage& img) const {
    if (img.isNull()) return;
    img = img.convertToFormat(QImage::Format_ARGB32);

    const int   kr  = m_key.red(), kg = m_key.green(), kb = m_key.blue();
    const float tol    = m_tolerance  * kMaxRGBDist;
    const float smooth = m_smoothness * kMaxRGBDist;

    for (int y = 0; y < img.height(); ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const QRgb px = line[x];
            const int dr = qRed(px)   - kr;
            const int dg = qGreen(px) - kg;
            const int db = qBlue(px)  - kb;
            const float dist = std::sqrt(static_cast<float>(dr*dr + dg*dg + db*db));

            float alpha;
            if (dist <= tol) {
                alpha = 0.0f;
            } else if (smooth < 1e-4f || dist >= tol + smooth) {
                alpha = 1.0f;
            } else {
                const float t = (dist - tol) / smooth;
                alpha = t * t * (3.0f - 2.0f * t);  // smoothstep
            }

            const int a = static_cast<int>(alpha * static_cast<float>(qAlpha(px)));
            line[x] = qRgba(qRed(px), qGreen(px), qBlue(px), a);
        }
    }
}

QJsonObject ChromaKeyFilter::toJson() const {
    return {
        {QStringLiteral("type"),       QStringLiteral("chroma_key")},
        {QStringLiteral("key"),        m_key.name()},
        {QStringLiteral("tolerance"),  static_cast<double>(m_tolerance)},
        {QStringLiteral("smoothness"), static_cast<double>(m_smoothness)},
    };
}

FilterEffect* ChromaKeyFilter::clone(QObject* parent) const {
    auto* f = new ChromaKeyFilter(parent);
    f->m_key        = m_key;
    f->m_tolerance  = m_tolerance;
    f->m_smoothness = m_smoothness;
    return f;
}

// ---------------------------------------------------------------------------
// BlurFilter — separable box blur with sliding window (O(W*H) per pass)
// ---------------------------------------------------------------------------

BlurFilter::BlurFilter(QObject* parent) : FilterEffect(parent) {}

void BlurFilter::setRadius(int r) {
    r = std::max(0, std::min(32, r));
    if (m_radius == r) return;
    m_radius = r;
    emit changed();
}

static void boxBlurRow(const QRgb* src, QRgb* dst, int w, int r) {
    // Replicate-padding sliding window: count is always (2r+1).
    const int diam = 2 * r + 1;
    int sumR = 0, sumG = 0, sumB = 0, sumA = 0;

    // Seed: window [-r..r], values < 0 replicate pixel[0].
    for (int k = -r; k <= r; ++k) {
        const QRgb px = src[std::max(0, k)];
        sumR += qRed(px);  sumG += qGreen(px);
        sumB += qBlue(px); sumA += qAlpha(px);
    }
    dst[0] = qRgba(sumR/diam, sumG/diam, sumB/diam, sumA/diam);

    for (int x = 1; x < w; ++x) {
        const int removeX = std::max(0,   x - r - 1);
        const int addX    = std::min(w-1, x + r);
        const QRgb rem = src[removeX], add = src[addX];
        sumR += qRed(add)   - qRed(rem);
        sumG += qGreen(add) - qGreen(rem);
        sumB += qBlue(add)  - qBlue(rem);
        sumA += qAlpha(add) - qAlpha(rem);
        dst[x] = qRgba(sumR/diam, sumG/diam, sumB/diam, sumA/diam);
    }
}

void BlurFilter::apply(QImage& img) const {
    if (m_radius <= 0 || img.isNull()) return;
    img = img.convertToFormat(QImage::Format_ARGB32);

    const int w = img.width(), h = img.height(), r = m_radius;

    // Horizontal pass: img → temp
    QImage temp(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y)
        boxBlurRow(reinterpret_cast<const QRgb*>(img.constScanLine(y)),
                   reinterpret_cast<QRgb*>(temp.scanLine(y)), w, r);

    // Vertical pass: temp → img (column-by-column, using a local buffer for cache
    // friendliness on the read side).
    const int diam = 2 * r + 1;
    std::vector<QRgb> col(static_cast<size_t>(h));
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y)
            col[static_cast<size_t>(y)] = reinterpret_cast<const QRgb*>(temp.constScanLine(y))[x];

        int sumR = 0, sumG = 0, sumB = 0, sumA = 0;
        for (int k = -r; k <= r; ++k) {
            const QRgb px = col[static_cast<size_t>(std::max(0, k))];
            sumR += qRed(px);  sumG += qGreen(px);
            sumB += qBlue(px); sumA += qAlpha(px);
        }
        reinterpret_cast<QRgb*>(img.scanLine(0))[x] = qRgba(sumR/diam, sumG/diam, sumB/diam, sumA/diam);

        for (int y = 1; y < h; ++y) {
            const int removeY = std::max(0,   y - r - 1);
            const int addY    = std::min(h-1, y + r);
            const QRgb rem = col[static_cast<size_t>(removeY)];
            const QRgb add = col[static_cast<size_t>(addY)];
            sumR += qRed(add)   - qRed(rem);
            sumG += qGreen(add) - qGreen(rem);
            sumB += qBlue(add)  - qBlue(rem);
            sumA += qAlpha(add) - qAlpha(rem);
            reinterpret_cast<QRgb*>(img.scanLine(y))[x] = qRgba(sumR/diam, sumG/diam, sumB/diam, sumA/diam);
        }
    }
}

QJsonObject BlurFilter::toJson() const {
    return {
        {QStringLiteral("type"),   QStringLiteral("blur")},
        {QStringLiteral("radius"), m_radius},
    };
}

FilterEffect* BlurFilter::clone(QObject* parent) const {
    auto* f = new BlurFilter(parent);
    f->m_radius = m_radius;
    return f;
}

// ---------------------------------------------------------------------------
// ScrollFilter — seamless wrap-around scroll
// ---------------------------------------------------------------------------

ScrollFilter::ScrollFilter(QObject* parent) : FilterEffect(parent) {}

void ScrollFilter::setSpeedX(float v) { if (m_speedX == v) return; m_speedX = v; emit changed(); }
void ScrollFilter::setSpeedY(float v) { if (m_speedY == v) return; m_speedY = v; emit changed(); }

void ScrollFilter::advance(qint64 nowUs) const {
    if (m_lastTickUs == 0) { m_lastTickUs = nowUs; return; }
    const float dtSec = static_cast<float>(nowUs - m_lastTickUs) * 1e-6f;
    m_offsetX = std::fmod(m_offsetX + m_speedX * dtSec, 65536.0f);
    m_offsetY = std::fmod(m_offsetY + m_speedY * dtSec, 65536.0f);
    m_lastTickUs = nowUs;
}

void ScrollFilter::apply(QImage& img) const {
    if (img.isNull()) return;
    const int w = img.width(), h = img.height();

    // Wrap offset into [0, w) and [0, h) — handle negative speeds correctly.
    const int ox = ((static_cast<int>(m_offsetX) % w) + w) % w;
    const int oy = ((static_cast<int>(m_offsetY) % h) + h) % h;
    if (ox == 0 && oy == 0) return;

    // Draw source with four tiles to produce seamless wrap.
    QImage result(w, h, img.format());
    result.fill(Qt::transparent);
    QPainter p(&result);
    p.drawImage(-ox,     -oy,     img);
    p.drawImage( w - ox, -oy,     img);
    p.drawImage(-ox,      h - oy, img);
    p.drawImage( w - ox,  h - oy, img);
    p.end();
    img = std::move(result);
}

QJsonObject ScrollFilter::toJson() const {
    return {
        {QStringLiteral("type"),   QStringLiteral("scroll")},
        {QStringLiteral("speedX"), static_cast<double>(m_speedX)},
        {QStringLiteral("speedY"), static_cast<double>(m_speedY)},
    };
}

FilterEffect* ScrollFilter::clone(QObject* parent) const {
    auto* f = new ScrollFilter(parent);
    f->m_speedX = m_speedX;
    f->m_speedY = m_speedY;
    // Intentionally do NOT copy runtime state (offset, lastTickUs).
    return f;
}
