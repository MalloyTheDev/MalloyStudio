#include "ui/components/PanelFrame.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

PanelFrame::PanelFrame(const QString& title, const QString& iconName, QWidget* parent)
    : QFrame(parent) {
    setObjectName(QStringLiteral("panel"));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* header = new QWidget(this);
    header->setObjectName(QStringLiteral("panelHeader"));
    header->setFixedHeight(36);
    m_headerRow = new QHBoxLayout(header);
    m_headerRow->setContentsMargins(12, 0, 8, 0);
    m_headerRow->setSpacing(8);

    if (!iconName.isEmpty() && Icons::has(iconName)) {
        auto* icon = new QLabel(header);
        icon->setPixmap(Icons::pixmap(iconName, Theme::TextDim, 14));
        m_headerRow->addWidget(icon);
    }

    m_title = new QLabel(title, header);
    m_title->setObjectName(QStringLiteral("panelTitle"));
    m_headerRow->addWidget(m_title);
    m_headerRow->addStretch();

    outer->addWidget(header);

    auto* bodyWrap = new QWidget(this);
    m_body = new QVBoxLayout(bodyWrap);
    m_body->setContentsMargins(0, 0, 0, 0);
    m_body->setSpacing(0);
    outer->addWidget(bodyWrap, 1);
}

void PanelFrame::setContent(QWidget* content) {
    if (!content) return;
    content->setParent(nullptr);
    m_body->addWidget(content, 1);
}

void PanelFrame::addHeaderWidget(QWidget* w) {
    if (w) m_headerRow->addWidget(w);
}
