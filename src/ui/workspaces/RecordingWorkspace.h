#pragma once

#include <QWidget>

// Recording Studio workspace (record.jsx): scenes + sources in a left column,
// preview + controls in the center, audio mixer + inspector on the right.
// It does not own the capture engine — it only arranges existing widgets that
// MainWindow creates and wires, so all current record/stream behavior is kept.
class RecordingWorkspace : public QWidget {
    Q_OBJECT
public:
    RecordingWorkspace(QWidget* scenes,
                       QWidget* sources,
                       QWidget* previewArea,
                       QWidget* controls,
                       QWidget* mixer,
                       QWidget* inspector,
                       QWidget* parent = nullptr);
};
