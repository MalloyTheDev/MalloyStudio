#pragma once

#include <QAbstractSpinBox>
#include <QComboBox>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QWidget>

// Whether the widget with focus is taking typed input, so a bare key press
// belongs to it rather than to an application-wide shortcut.
//
// Kept apart from AppShell so the decision can be tested without a window.
// The case that went wrong is the reason it is broader than a line edit: a
// spin box makes itself the focus proxy of its own line edit, so while one is
// focused the focus widget is the spin box, and checking for QLineEdit alone
// let digits typed into width, bitrate and position fields switch workspaces.
inline bool isTakingTypedInput(const QWidget* w) {
    if (!w) return false;
    if (qobject_cast<const QLineEdit*>(w) || qobject_cast<const QAbstractSpinBox*>(w)
        || qobject_cast<const QTextEdit*>(w) || qobject_cast<const QPlainTextEdit*>(w)
        || qobject_cast<const QKeySequenceEdit*>(w))
        return true;
    if (const auto* combo = qobject_cast<const QComboBox*>(w))
        return combo->isEditable();
    // Anything else that accepts composed text from an input method is also
    // being typed into, whatever its class.
    return w->testAttribute(Qt::WA_InputMethodEnabled);
}
