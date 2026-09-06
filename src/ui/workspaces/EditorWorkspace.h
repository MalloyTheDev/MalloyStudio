#pragma once

#include <QWidget>

class MediaRegistry;
class QJsonArray;
class QLabel;

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

    // Reflects the document's real save state in the timeline toolbar.
    // hasFile is false for a project that has never been saved.
    void setSaveState(bool hasFile, bool modified);

signals:
    // Export was clicked. MainWindow owns the output-file choice and the render
    // queue, so the workspace only reports the intent.
    void exportRequested();

    // A clip was added, moved, trimmed, split or deleted. MainWindow marks the
    // document modified, which is what makes the close prompt and the toolbar
    // state tell the truth about unsaved timeline work.
    void timelineChanged();

private:
    QWidget*       m_timelineCanvas = nullptr;   // a TimelineCanvas (defined in the .cpp)
    MediaRegistry* m_media          = nullptr;
    QLabel*        m_saveDot        = nullptr;   // toolbar state indicator
    QLabel*        m_saveLabel      = nullptr;
};
