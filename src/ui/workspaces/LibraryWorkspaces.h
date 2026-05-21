#pragma once

#include <QWidget>

// The four "library" workspaces, which share a left-filter / content pattern
// (libraries.jsx + secondary.jsx RenderQueue). Demo content only — the backing
// registries (clips, media index, project index, render queue) come later.

class ClipsWorkspace : public QWidget {
    Q_OBJECT
public:
    explicit ClipsWorkspace(QWidget* parent = nullptr);
};

class MediaWorkspace : public QWidget {
    Q_OBJECT
public:
    explicit MediaWorkspace(QWidget* parent = nullptr);
};

class ProjectsWorkspace : public QWidget {
    Q_OBJECT
public:
    explicit ProjectsWorkspace(QWidget* parent = nullptr);
};

class RenderWorkspace : public QWidget {
    Q_OBJECT
public:
    explicit RenderWorkspace(QWidget* parent = nullptr);
};
