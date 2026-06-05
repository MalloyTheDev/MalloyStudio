#pragma once

#include <QWidget>

class MediaRegistry;
class QJsonArray;

// Video Editor workspace (editor.jsx): media bin + preview monitor + inspector
// in resizable splits over a custom-painted multi-track timeline. Edit ops
// (move/trim/razor/cross-track) live in the timeline; the clip layout persists
// inside the active project (.malloy.json) via timelineJson()/setTimelineJson().
//
// MediaRegistry backs the Media Bin (live registry-driven list + drag source).
// nullptr is accepted for tests / fallback; the bin then shows an empty state.
class EditorWorkspace : public QWidget {
    Q_OBJECT
public:
    explicit EditorWorkspace(MediaRegistry* media = nullptr, QWidget* parent = nullptr);

    // Editor timeline as JSON, for round-tripping through ProjectDocument.
    QJsonArray timelineJson() const;
    void setTimelineJson(const QJsonArray& timeline);

private:
    QWidget*       m_timelineCanvas = nullptr;   // a TimelineCanvas (defined in the .cpp)
    MediaRegistry* m_media          = nullptr;
};
