#include "ui/shell/WorkspaceHeader.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"

#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

WorkspaceHeader::WorkspaceHeader(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("workspaceHeader"));
    setFixedHeight(44);

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(16, 0, 16, 0);
    row->setSpacing(8);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("wsTitle"));
    row->addWidget(m_title, 0, Qt::AlignVCenter);

    m_subtitle = new QLabel(this);
    m_subtitle->setObjectName(QStringLiteral("wsSubtitle"));
    row->addWidget(m_subtitle, 0, Qt::AlignVCenter);

    row->addStretch();

    // Command palette pill
    auto* cmd = new QPushButton(QStringLiteral("Search or run command…"), this);
    cmd->setObjectName(QStringLiteral("headerPill"));
    cmd->setIcon(Icons::icon(QStringLiteral("search"), Theme::TextMute, 14));
    cmd->setIconSize(QSize(14, 14));
    cmd->setMinimumWidth(220);
    cmd->setCursor(Qt::PointingHandCursor);
    cmd->setToolTip(QStringLiteral("Command palette   Ctrl+K"));
    cmd->setLayoutDirection(Qt::LeftToRight);
    connect(cmd, &QPushButton::clicked, this, &WorkspaceHeader::commandPaletteRequested);
    row->addWidget(cmd, 0, Qt::AlignVCenter);

    // Project pill
    m_projectPill = new QPushButton(this);
    m_projectPill->setObjectName(QStringLiteral("headerPill"));
    m_projectPill->setIcon(Icons::icon(QStringLiteral("folder"), Theme::TextDim, 14));
    m_projectPill->setIconSize(QSize(14, 14));
    m_projectPill->setCursor(Qt::PointingHandCursor);
    m_projectPill->setToolTip(QStringLiteral("Open project"));
    connect(m_projectPill, &QPushButton::clicked, this, &WorkspaceHeader::projectPillClicked);
    row->addWidget(m_projectPill, 0, Qt::AlignVCenter);

    setProjectName(QStringLiteral("Untitled"));
}

void WorkspaceHeader::setTitle(const QString& title) {
    m_title->setText(title);
}

void WorkspaceHeader::setSubtitle(const QString& subtitle) {
    m_subtitle->setText(subtitle);
    m_subtitle->setVisible(!subtitle.isEmpty());
}

void WorkspaceHeader::setProjectName(const QString& name) {
    m_projectName = name;
    const QString full = name.isEmpty() ? QStringLiteral("Untitled") : name;
    const QFontMetrics fm(m_projectPill->font());
    const QString elided = fm.elidedText(full, Qt::ElideRight, 240);
    m_projectPill->setText(QStringLiteral("  %1  ▾").arg(elided));
}
