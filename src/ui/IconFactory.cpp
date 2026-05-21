#include "ui/IconFactory.h"

#include <QGuiApplication>
#include <QHash>
#include <QPainter>
#include <QScreen>
#include <QSvgRenderer>

namespace {

// Inner SVG fragments (24x24 viewBox), transcribed from primitives.jsx ICONS.
// fill="currentColor" placeholders are substituted with the requested color.
const QHash<QString, QString>& fragments() {
    static const QHash<QString, QString> kIcons = {
        {"dashboard", R"(<rect x="3" y="3" width="7" height="7" rx="1.5"/><rect x="14" y="3" width="7" height="4" rx="1.5"/><rect x="14" y="10" width="7" height="11" rx="1.5"/><rect x="3" y="13" width="7" height="8" rx="1.5"/>)"},
        {"record", R"(<circle cx="12" cy="12" r="8"/><circle cx="12" cy="12" r="3.5" fill="currentColor" stroke="none"/>)"},
        {"stream", R"(<path d="M5 7c0 4 4 6 7 6s7-2 7-6"/><path d="M5 17c0-4 4-6 7-6s7 2 7 6"/>)"},
        {"editor", R"(<rect x="3" y="6" width="18" height="4" rx="1"/><rect x="3" y="14" width="18" height="4" rx="1"/><circle cx="8" cy="8" r="1" fill="currentColor"/><circle cx="16" cy="16" r="1" fill="currentColor"/>)"},
        {"clips", R"(<rect x="3" y="5" width="13" height="14" rx="1.5"/><path d="M16 9l5-3v12l-5-3"/>)"},
        {"media", R"(<rect x="3" y="5" width="18" height="14" rx="1.5"/><path d="M3 15l5-4 4 3 3-2 6 5"/>)"},
        {"projects", R"(<path d="M3 6.5C3 5.7 3.7 5 4.5 5h4l2 2h9c.8 0 1.5.7 1.5 1.5v8c0 .8-.7 1.5-1.5 1.5h-15c-.8 0-1.5-.7-1.5-1.5V6.5z"/>)"},
        {"render", R"(<rect x="3" y="4" width="18" height="6" rx="1.5"/><rect x="3" y="14" width="18" height="6" rx="1.5"/><circle cx="7" cy="7" r="1" fill="currentColor"/><circle cx="7" cy="17" r="1" fill="currentColor"/>)"},
        {"ai", R"(<path d="M12 3l2.5 5 5.5.8-4 3.9.9 5.5L12 15.6 7.1 18.2 8 12.7 4 8.8l5.5-.8z"/>)"},
        {"settings", R"(<circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.7 1.7 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-1.8-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1.1-1.5 1.7 1.7 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.8 1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.5-1.1 1.7 1.7 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.8.3H9a1.7 1.7 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.8V9a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1z"/>)"},
        {"plus", R"(<path d="M12 5v14M5 12h14"/>)"},
        {"minus", R"(<path d="M5 12h14"/>)"},
        {"search", R"(<circle cx="11" cy="11" r="6.5"/><path d="M20 20l-4.3-4.3"/>)"},
        {"filter", R"(<path d="M4 5h16l-6 8v6l-4-2v-4z"/>)"},
        {"grid", R"(<rect x="3" y="3" width="7" height="7" rx="1"/><rect x="14" y="3" width="7" height="7" rx="1"/><rect x="3" y="14" width="7" height="7" rx="1"/><rect x="14" y="14" width="7" height="7" rx="1"/>)"},
        {"list", R"(<path d="M8 6h13M8 12h13M8 18h13M3 6h.01M3 12h.01M3 18h.01"/>)"},
        {"more", R"(<circle cx="5" cy="12" r="1" fill="currentColor"/><circle cx="12" cy="12" r="1" fill="currentColor"/><circle cx="19" cy="12" r="1" fill="currentColor"/>)"},
        {"play", R"(<path d="M7 5v14l12-7z" fill="currentColor"/>)"},
        {"pause", R"(<rect x="6" y="5" width="4" height="14" fill="currentColor" stroke="none"/><rect x="14" y="5" width="4" height="14" fill="currentColor" stroke="none"/>)"},
        {"stop", R"(<rect x="6" y="6" width="12" height="12" fill="currentColor" stroke="none" rx="1"/>)"},
        {"skipFwd", R"(<path d="M5 5v14l10-7zM17 5v14"/>)"},
        {"skipBack", R"(<path d="M19 5v14L9 12zM7 5v14"/>)"},
        {"upload", R"(<path d="M12 16V4M5 11l7-7 7 7M4 20h16"/>)"},
        {"download", R"(<path d="M12 4v12M5 11l7 7 7-7M4 20h16"/>)"},
        {"trash", R"(<path d="M4 7h16M9 7V5a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v2M6 7l1 12a2 2 0 0 0 2 2h6a2 2 0 0 0 2-2l1-12"/>)"},
        {"star", R"(<path d="M12 4l2.5 5 5.5.8-4 3.9.9 5.5L12 16.6 7.1 19.2 8 13.7 4 9.8l5.5-.8z"/>)"},
        {"folder", R"(<path d="M3 6.5C3 5.7 3.7 5 4.5 5h4l2 2h9c.8 0 1.5.7 1.5 1.5v8c0 .8-.7 1.5-1.5 1.5h-15c-.8 0-1.5-.7-1.5-1.5V6.5z"/>)"},
        {"cut", R"(<circle cx="6" cy="6" r="2.5"/><circle cx="6" cy="18" r="2.5"/><path d="M8 8l12 8M8 16l12-8"/>)"},
        {"layers", R"(<path d="M12 4l9 4.5-9 4.5-9-4.5zM3 13l9 4.5 9-4.5M3 17.5l9 4.5 9-4.5"/>)"},
        {"mic", R"(<rect x="9" y="3" width="6" height="12" rx="3"/><path d="M5 11a7 7 0 0 0 14 0M12 18v3M9 21h6"/>)"},
        {"micOff", R"(<path d="M9 3.5a3 3 0 0 1 6 0v6M9 9v.5a3 3 0 0 0 4.7 2.5M5 11a7 7 0 0 0 11.5 5.3M12 18v3M9 21h6M3 3l18 18"/>)"},
        {"camera", R"(<rect x="3" y="6" width="13" height="12" rx="1.5"/><path d="M16 10l5-3v10l-5-3z"/>)"},
        {"display", R"(<rect x="3" y="4" width="18" height="12" rx="1.5"/><path d="M9 20h6M12 16v4"/>)"},
        {"window", R"(<rect x="3" y="5" width="18" height="14" rx="1.5"/><path d="M3 9h18"/>)"},
        {"text", R"(<path d="M5 5h14M12 5v14M9 19h6"/>)"},
        {"image", R"(<rect x="3" y="5" width="18" height="14" rx="1.5"/><circle cx="9" cy="10" r="1.5"/><path d="M21 15l-5-4-4 3-3-2-6 5"/>)"},
        {"browser", R"(<rect x="3" y="5" width="18" height="14" rx="1.5"/><path d="M3 9h18M6.5 7h.01M9 7h.01"/>)"},
        {"speaker", R"(<path d="M5 10v4h3l5 4V6L8 10zM17 8a5 5 0 0 1 0 8M19.5 5a9 9 0 0 1 0 14"/>)"},
        {"eye", R"(<path d="M2 12s4-7 10-7 10 7 10 7-4 7-10 7-10-7-10-7z"/><circle cx="12" cy="12" r="2.5"/>)"},
        {"eyeOff", R"(<path d="M3 3l18 18M10 6c.6-.1 1.3-.2 2-.2 6 0 10 6.2 10 6.2a16 16 0 0 1-2.6 3.2M6.6 6.8C4 8.6 2 12 2 12s4 7 10 7c2 0 3.7-.7 5-1.7"/>)"},
        {"lock", R"(<rect x="5" y="11" width="14" height="9" rx="1.5"/><path d="M8 11V7a4 4 0 0 1 8 0v4"/>)"},
        {"unlock", R"(<rect x="5" y="11" width="14" height="9" rx="1.5"/><path d="M8 11V7a4 4 0 0 1 7.5-2"/>)"},
        {"chevDown", R"(<path d="M6 9l6 6 6-6"/>)"},
        {"chevRight", R"(<path d="M9 6l6 6-6 6"/>)"},
        {"chevLeft", R"(<path d="M15 6l-6 6 6 6"/>)"},
        {"close", R"(<path d="M6 6l12 12M18 6L6 18"/>)"},
        {"check", R"(<path d="M5 12l5 5 9-11"/>)"},
        {"alert", R"(<path d="M12 3l10 18H2z"/><path d="M12 10v5M12 18v.01"/>)"},
        {"info", R"(<circle cx="12" cy="12" r="9"/><path d="M12 11v6M12 8v.01"/>)"},
        {"bell", R"(<path d="M6 9a6 6 0 0 1 12 0c0 5 2 6 2 6H4s2-1 2-6zM10 19a2 2 0 0 0 4 0"/>)"},
        {"cpu", R"(<rect x="6" y="6" width="12" height="12" rx="1.5"/><rect x="9" y="9" width="6" height="6" rx="0.5"/><path d="M9 3v3M15 3v3M9 18v3M15 18v3M3 9h3M3 15h3M18 9h3M18 15h3"/>)"},
        {"disk", R"(<circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="2.5"/>)"},
        {"link", R"(<path d="M10 14a4 4 0 0 0 5.7 0l3-3a4 4 0 1 0-5.7-5.7l-1 1M14 10a4 4 0 0 0-5.7 0l-3 3a4 4 0 1 0 5.7 5.7l1-1"/>)"},
        {"scissors", R"(<circle cx="6" cy="6" r="2.5"/><circle cx="6" cy="18" r="2.5"/><path d="M8 8l12 8M8 16l12-8"/>)"},
        {"drag", R"(<circle cx="9" cy="6" r="1" fill="currentColor"/><circle cx="15" cy="6" r="1" fill="currentColor"/><circle cx="9" cy="12" r="1" fill="currentColor"/><circle cx="15" cy="12" r="1" fill="currentColor"/><circle cx="9" cy="18" r="1" fill="currentColor"/><circle cx="15" cy="18" r="1" fill="currentColor"/>)"},
        {"command", R"(<path d="M9 6a3 3 0 1 0 0 6h6a3 3 0 1 0 0-6 3 3 0 0 0-3 3v6a3 3 0 1 1-3 3 3 3 0 0 1 3-3h6a3 3 0 1 1-3 3"/>)"},
        {"zoomIn", R"(<circle cx="11" cy="11" r="6.5"/><path d="M20 20l-4.3-4.3M11 8v6M8 11h6"/>)"},
        {"zoomOut", R"(<circle cx="11" cy="11" r="6.5"/><path d="M20 20l-4.3-4.3M8 11h6"/>)"},
        {"refresh", R"(<path d="M21 12a9 9 0 1 1-3-6.7L21 8M21 3v5h-5"/>)"},
        {"bolt", R"(<path d="M13 3L4 14h7l-1 7 9-11h-7z"/>)"},
        {"sparkle", R"(<path d="M12 4v6M12 14v6M4 12h6M14 12h6M6 6l3 3M15 15l3 3M18 6l-3 3M9 15l-3 3"/>)"},
        {"question", R"(<circle cx="12" cy="12" r="9"/><path d="M9.5 9a2.5 2.5 0 1 1 3.5 2.3c-.7.4-1 1-1 1.7M12 17v.01"/>)"},
    };
    return kIcons;
}

} // namespace

namespace Icons {

bool has(const QString& name) {
    return fragments().contains(name);
}

QPixmap pixmap(const QString& name, const QColor& color, int size, qreal strokeWidth) {
    const QString frag = fragments().value(name);
    if (frag.isEmpty()) return {};

    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    const int backing = qMax(1, qRound(size * dpr));

    static QHash<QString, QPixmap> cache;
    const QString key = QStringLiteral("%1|%2|%3|%4|%5")
                            .arg(name, color.name(QColor::HexArgb))
                            .arg(size).arg(strokeWidth).arg(dpr);
    auto it = cache.constFind(key);
    if (it != cache.constEnd()) return it.value();

    QString body = frag;
    body.replace(QStringLiteral("currentColor"), color.name());

    const QString svg = QStringLiteral(
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' "
        "fill='none' stroke='%1' stroke-width='%2' "
        "stroke-linecap='round' stroke-linejoin='round'>%3</svg>")
        .arg(color.name()).arg(strokeWidth).arg(body);

    QSvgRenderer renderer(svg.toUtf8());
    QPixmap pm(backing, backing);
    pm.fill(Qt::transparent);
    {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        renderer.render(&p, QRectF(0, 0, backing, backing));
    }
    pm.setDevicePixelRatio(dpr);
    cache.insert(key, pm);
    return pm;
}

QIcon icon(const QString& name, const QColor& color, int size, qreal strokeWidth) {
    return QIcon(pixmap(name, color, size, strokeWidth));
}

QPixmap renderSvg(const QString& svgMarkup, int size) {
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    const int backing = qMax(1, qRound(size * dpr));
    QSvgRenderer renderer(svgMarkup.toUtf8());
    QPixmap pm(backing, backing);
    pm.fill(Qt::transparent);
    {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        renderer.render(&p, QRectF(0, 0, backing, backing));
    }
    pm.setDevicePixelRatio(dpr);
    return pm;
}

} // namespace Icons
