#include "ui/shell/IconRail.h"
#include "ui/shell/NavModel.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"

#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

namespace {
constexpr int kRailWidth = 56;
constexpr int kButtonH   = 40;
constexpr int kIconSize  = 18;

QString brandMarkSvg() {
    // Geometric mark from shell.jsx BrandMark — rounded square + stylized M + accent dot.
    return QStringLiteral(
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'>"
        "<rect x='3' y='3' width='26' height='26' rx='6' fill='%1' stroke='%2'/>"
        "<path d='M9 22V10l4 6 3-5 3 5 4-6v12' stroke='%3' stroke-width='2' fill='none' "
        "stroke-linecap='round' stroke-linejoin='round'/>"
        "<circle cx='23' cy='9' r='2' fill='%4'/></svg>")
        .arg(Theme::Surface2.name(), Theme::BorderStrong.name(),
             Theme::Text.name(), Theme::Accent.name());
}
} // namespace

IconRail::IconRail(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("iconRail"));
    setFixedWidth(kRailWidth);

    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(0, 8, 0, 0);
    col->setSpacing(0);

    // Brand mark
    auto* brand = new QLabel(this);
    brand->setFixedHeight(44);
    brand->setAlignment(Qt::AlignCenter);
    brand->setPixmap(Icons::renderSvg(brandMarkSvg(), 22));
    col->addWidget(brand);
    col->addSpacing(6);

    // Workspace buttons
    auto* navWrap = new QWidget(this);
    auto* navCol = new QVBoxLayout(navWrap);
    navCol->setContentsMargins(6, 0, 6, 0);
    navCol->setSpacing(2);
    for (const NavItem& n : navItems()) {
        auto* b = new QPushButton(navWrap);
        b->setObjectName(QStringLiteral("railButton"));
        b->setFixedHeight(kButtonH);
        b->setCursor(Qt::PointingHandCursor);
        b->setIconSize(QSize(kIconSize, kIconSize));
        b->setProperty("iconName", n.icon);
        b->setToolTip(n.planned
            ? QStringLiteral("%1 · planned   %2").arg(n.label, n.kbd)
            : QStringLiteral("%1   %2").arg(n.label, n.kbd));
        const QString id = n.id;
        connect(b, &QPushButton::clicked, this, [this, id] { emit picked(id); });
        navCol->addWidget(b);
        m_buttons.insert(n.id, b);
    }
    col->addWidget(navWrap);

    col->addStretch();

    // Help + avatar
    auto* footer = new QWidget(this);
    auto* fcol = new QVBoxLayout(footer);
    fcol->setContentsMargins(6, 8, 6, 8);
    fcol->setSpacing(4);

    auto* help = new QPushButton(footer);
    help->setObjectName(QStringLiteral("railButton"));
    help->setFixedHeight(36);
    help->setIconSize(QSize(16, 16));
    help->setIcon(Icons::icon(QStringLiteral("question"), Theme::TextMute, 16));
    help->setToolTip(QStringLiteral("Help & shortcuts"));
    fcol->addWidget(help);

    auto* avatar = new QLabel(QStringLiteral("JM"), footer);
    avatar->setObjectName(QStringLiteral("railAvatar"));
    avatar->setFixedHeight(32);
    avatar->setAlignment(Qt::AlignCenter);
    fcol->addWidget(avatar);

    col->addWidget(footer);

    if (!navItems().isEmpty()) setActiveWorkspace(navItems().first().id);
}

void IconRail::setActiveWorkspace(const QString& id) {
    if (m_active == id) return;
    m_active = id;
    for (auto it = m_buttons.constBegin(); it != m_buttons.constEnd(); ++it) {
        Theme::setProp(it.value(), "active", it.key() == id);
        Theme::repolish(it.value());
    }
    retint();
    update();
}

void IconRail::retint() {
    // Active button gets full-strength icon; others dim. Planned items stay dim.
    const auto& items = navItems();
    for (const NavItem& n : items) {
        QPushButton* b = m_buttons.value(n.id);
        if (!b) continue;
        const bool active = (n.id == m_active);
        const QColor c = active ? Theme::Text : Theme::TextDim;
        b->setIcon(Icons::icon(n.icon, c, kIconSize));
    }
}

void IconRail::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);
    QPushButton* b = m_buttons.value(m_active);
    if (!b) return;
    // Accent bar at the left edge, vertically centered on the active button.
    const QRect g = b->geometry();
    QPoint topLeft = b->parentWidget()->mapTo(this, g.topLeft());
    const int barH = 28;
    const int y = topLeft.y() + (g.height() - barH) / 2;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(Theme::Accent);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(QRectF(0, y, 3, barH), 1.5, 1.5);
}
