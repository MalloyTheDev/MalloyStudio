#pragma once

// Geometric stroke icon set, ported from MalloyStudioJS/primitives.jsx (ICONS).
//
// Each icon is stored as the inner SVG fragment of a 24x24 viewBox. icon()/
// pixmap() wrap it in a full <svg> with the requested stroke color, render via
// QSvgRenderer (no image-format plugin needed — we link Qt6::Svg directly),
// and tint any fill="currentColor" to the same color. Results are cached.

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>

namespace Icons {

// Tinted pixmap at logical `size` px (HiDPI-aware via devicePixelRatio).
QPixmap pixmap(const QString& name, const QColor& color, int size = 16, qreal strokeWidth = 1.5);

// Convenience QIcon wrapping pixmap().
QIcon icon(const QString& name, const QColor& color, int size = 16, qreal strokeWidth = 1.5);

// Render an arbitrary SVG document string to a tinted-by-its-own-fills pixmap
// (HiDPI-aware). Used for one-off marks like the brand logo.
QPixmap renderSvg(const QString& svgMarkup, int size);

bool has(const QString& name);

} // namespace Icons
