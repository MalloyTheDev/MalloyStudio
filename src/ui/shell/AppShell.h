#pragma once

#include <QHash>
#include <QString>
#include <QWidget>

class IconRail;
class WorkspaceHeader;
class StudioStatusBar;
class QStackedWidget;

// Top-level chrome: icon rail (left) + workspace column (header over a stacked
// body) + full-width status bar (bottom). Mirrors the .app / .app-body /
// .workspace grid in tokens.css. MainWindow sets this as its central widget.
class AppShell : public QWidget {
    Q_OBJECT
public:
    explicit AppShell(QWidget* parent = nullptr);

    // Register a workspace page under a NavItem id. Takes ownership.
    void addWorkspace(const QString& id, QWidget* page);
    void setCurrentWorkspace(const QString& id);
    QString currentWorkspace() const { return m_current; }

    IconRail*        rail()    const { return m_rail; }
    WorkspaceHeader* header()  const { return m_header; }
    StudioStatusBar* status()  const { return m_status; }

signals:
    void workspaceChanged(const QString& id);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void selectWorkspace(const QString& id);

    IconRail*        m_rail = nullptr;
    WorkspaceHeader* m_header = nullptr;
    StudioStatusBar* m_status = nullptr;
    QStackedWidget*  m_stack = nullptr;
    QHash<QString, int> m_pageIndex;
    QString m_current;
};
