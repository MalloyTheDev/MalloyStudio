#include "ui/shell/AppShell.h"
#include "ui/shell/IconRail.h"
#include "ui/shell/NavModel.h"
#include "ui/shell/StudioStatusBar.h"
#include "ui/shell/WorkspaceHeader.h"

#include <QApplication>
#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace {
QString subtitleFor(const QString& id) {
    if (id == QLatin1String("dashboard")) return QStringLiteral("Project hub");
    if (id == QLatin1String("record"))    return QStringLiteral("Live composing");
    if (id == QLatin1String("stream"))     return QStringLiteral("Twitch · /malloy_live");
    if (id == QLatin1String("editor"))     return QStringLiteral("Timeline");
    if (id == QLatin1String("clips"))      return QStringLiteral("Replay-buffer captures");
    if (id == QLatin1String("media"))      return QStringLiteral("Project media");
    if (id == QLatin1String("projects"))   return QStringLiteral("All projects");
    if (id == QLatin1String("render"))     return QStringLiteral("Background renders");
    if (id == QLatin1String("settings"))   return QStringLiteral("App preferences");
    if (id == QLatin1String("ai"))         return QStringLiteral("Preview · planned features");
    return QString();
}
QString labelFor(const QString& id) {
    for (const NavItem& n : navItems())
        if (n.id == id) return n.label;
    return id;
}
} // namespace

AppShell::AppShell(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* body = new QWidget(this);
    auto* bodyRow = new QHBoxLayout(body);
    bodyRow->setContentsMargins(0, 0, 0, 0);
    bodyRow->setSpacing(0);

    m_rail = new IconRail(body);
    bodyRow->addWidget(m_rail);

    auto* col = new QWidget(body);
    auto* colV = new QVBoxLayout(col);
    colV->setContentsMargins(0, 0, 0, 0);
    colV->setSpacing(0);

    m_header = new WorkspaceHeader(col);
    colV->addWidget(m_header);

    m_stack = new QStackedWidget(col);
    m_stack->setObjectName(QStringLiteral("workspaceBody"));
    colV->addWidget(m_stack, 1);

    bodyRow->addWidget(col, 1);
    outer->addWidget(body, 1);

    m_status = new StudioStatusBar(this);
    outer->addWidget(m_status);

    connect(m_rail, &IconRail::picked, this, &AppShell::selectWorkspace);

    qApp->installEventFilter(this);  // digit-key (1..0) workspace switching
}

void AppShell::addWorkspace(const QString& id, QWidget* page) {
    const int idx = m_stack->addWidget(page);
    m_pageIndex.insert(id, idx);
    if (m_current.isEmpty()) selectWorkspace(id);
}

void AppShell::setCurrentWorkspace(const QString& id) {
    selectWorkspace(id);
}

void AppShell::selectWorkspace(const QString& id) {
    auto it = m_pageIndex.constFind(id);
    if (it == m_pageIndex.constEnd()) return;
    m_current = id;
    m_stack->setCurrentIndex(it.value());
    m_rail->setActiveWorkspace(id);
    m_header->setTitle(labelFor(id));
    m_header->setSubtitle(subtitleFor(id));
    emit workspaceChanged(id);
}

bool AppShell::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (!(ke->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
            // Don't steal digits while typing into an editable widget.
            QWidget* fw = QApplication::focusWidget();
            const bool editing = qobject_cast<QLineEdit*>(fw)
                || (qobject_cast<QComboBox*>(fw) && qobject_cast<QComboBox*>(fw)->isEditable());
            if (!editing) {
                const QString text = ke->text();
                if (text.size() == 1 && text.at(0).isDigit()) {
                    for (const NavItem& n : navItems()) {
                        if (n.kbd == text) { selectWorkspace(n.id); return true; }
                    }
                }
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}
