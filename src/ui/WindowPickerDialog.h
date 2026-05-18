#pragma once
#include <QDialog>
#include <QString>
#include <optional>
#include <utility>

class QListWidget;

// Enumerates visible top-level windows and lets the user select one to capture.
// Returns (HWND as quintptr, window title) on accept, or nullopt on cancel.
class WindowPickerDialog : public QDialog {
    Q_OBJECT
public:
    explicit WindowPickerDialog(QWidget* parent = nullptr);

    // Convenience: show the dialog and return the selection.
    // Returns nullopt if the user cancelled or no window was selected.
    static std::optional<std::pair<quintptr, QString>> pickWindow(QWidget* parent);

    quintptr selectedHwnd()  const;
    QString  selectedTitle() const;

private:
    void populate(quintptr selfHwnd);

    QListWidget*   m_list;
    QList<quintptr> m_hwnds;
    QList<QString>  m_titles;
};
