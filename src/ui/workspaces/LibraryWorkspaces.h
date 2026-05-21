#pragma once

#include <QWidget>

// The four "library" workspaces, which share a left-filter / content pattern
// (libraries.jsx + secondary.jsx RenderQueue). Clips is backed by ClipsRegistry;
// the others still use demo content pending their registries.

class ClipsRegistry;
class ProjectRegistry;
class QLabel;
class QScrollArea;

class ClipsWorkspace : public QWidget {
    Q_OBJECT
public:
    explicit ClipsWorkspace(ClipsRegistry* registry, QWidget* parent = nullptr);

private:
    void rebuild();

    ClipsRegistry* m_registry = nullptr;
    QScrollArea* m_scroll = nullptr;
    QLabel* m_countLabel = nullptr;
};

class MediaWorkspace : public QWidget {
    Q_OBJECT
public:
    explicit MediaWorkspace(QWidget* parent = nullptr);
};

class ProjectsWorkspace : public QWidget {
    Q_OBJECT
public:
    explicit ProjectsWorkspace(ProjectRegistry* registry, QWidget* parent = nullptr);

signals:
    void openRequested(const QString& filePath);
    void newRequested();

private:
    void rebuild();

    ProjectRegistry* m_registry = nullptr;
    QScrollArea* m_scroll = nullptr;
    QLabel* m_countLabel = nullptr;
};

class RenderWorkspace : public QWidget {
    Q_OBJECT
public:
    explicit RenderWorkspace(QWidget* parent = nullptr);
};
