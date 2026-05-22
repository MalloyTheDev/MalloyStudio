#pragma once

#include <QWidget>

class QJsonArray;

// Video Editor workspace (editor.jsx): media bin + preview monitor + inspector
// in resizable splits over a custom-painted multi-track timeline. Edit ops
// (move/trim/razor/cross-track) live in the timeline; the clip layout persists
// inside the active project (.malloy.json) via timelineJson()/setTimelineJson().
class EditorWorkspace : public QWidget {
    Q_OBJECT
public:
    explicit EditorWorkspace(QWidget* parent = nullptr);

    // Editor timeline as JSON, for round-tripping through ProjectDocument.
    QJsonArray timelineJson() const;
    void setTimelineJson(const QJsonArray& timeline);

private:
    QWidget* m_timelineCanvas = nullptr;   // a TimelineCanvas (defined in the .cpp)
};
