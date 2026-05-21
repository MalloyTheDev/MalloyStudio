#pragma once

#include <QString>
#include <QWidget>

// Centered "coming soon" page for workspaces not yet built in this increment
// (Streaming, Editor, Clips, Media, Projects, Render Queue, AI Lab, Settings).
class PlaceholderWorkspace : public QWidget {
    Q_OBJECT
public:
    PlaceholderWorkspace(const QString& iconName, const QString& title,
                         const QString& subtitle, QWidget* parent = nullptr);
};
