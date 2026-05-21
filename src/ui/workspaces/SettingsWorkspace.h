#pragma once

#include <QWidget>

class QStackedWidget;

// Settings workspace (secondary.jsx SettingsScreen): a two-pane screen with a
// group list on the left and a content area on the right. The Recording group
// is built out in full; other groups share a generic same-pattern page. This
// is the surface that will eventually consolidate the standalone dialogs.
class SettingsWorkspace : public QWidget {
    Q_OBJECT
public:
    explicit SettingsWorkspace(QWidget* parent = nullptr);

private:
    QWidget* buildRecordingPage();
    QWidget* buildGenericPage(const QString& title);

    QStackedWidget* m_stack = nullptr;
};
