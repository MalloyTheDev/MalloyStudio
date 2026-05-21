#pragma once

#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;

// Top bar of a workspace: title + subtitle on the left, a command-palette pill
// and project pill on the right. Mirrors WorkspaceHeader in shell.jsx.
class WorkspaceHeader : public QWidget {
    Q_OBJECT
public:
    explicit WorkspaceHeader(QWidget* parent = nullptr);

    void setTitle(const QString& title);
    void setSubtitle(const QString& subtitle);
    void setProjectName(const QString& name);

signals:
    void commandPaletteRequested();
    void projectPillClicked();

private:
    QLabel* m_title = nullptr;
    QLabel* m_subtitle = nullptr;
    QPushButton* m_projectPill = nullptr;
    QString m_projectName;
};
