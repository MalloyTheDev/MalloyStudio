#pragma once

#include <QWidget>

// Video Editor workspace (editor.jsx): media bin + preview monitor + inspector
// in resizable splits over a custom-painted multi-track timeline. This is the
// v0.2 centerpiece; here it's a faithful visual conversion (playhead scrub,
// clip selection, zoom) — deep editing operations come later.
class EditorWorkspace : public QWidget {
    Q_OBJECT
public:
    explicit EditorWorkspace(QWidget* parent = nullptr);
};
