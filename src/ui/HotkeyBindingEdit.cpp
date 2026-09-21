#include "ui/HotkeyBindingEdit.h"

#include "input/HotkeyManager.h"

#include <QSignalBlocker>

HotkeyBindingEdit::HotkeyBindingEdit(const QString& actionId, QWidget* parent)
    : QKeySequenceEdit(parent), m_actionId(actionId) {
    // The manager registers only the first key combination of a sequence, so
    // the field accepts one, which also ends the edit as soon as it is let go.
    setMaximumSequenceLength(1);
    setEnabled(false);
    // Applied when the edit is finished, not on keySequenceChanged: the field
    // clears itself as the first key goes down, and taking that as the user's
    // choice unbound the action before the new shortcut was known, so a
    // shortcut that was then refused left nothing bound at all.
    connect(this, &QKeySequenceEdit::editingFinished, this, &HotkeyBindingEdit::applyEdit);
}

void HotkeyBindingEdit::setManager(HotkeyManager* manager) {
    m_manager = manager;
    setEnabled(manager != nullptr);
    showHeldBinding();
}

void HotkeyBindingEdit::showHeldBinding() {
    const QKeySequence held = m_manager ? m_manager->binding(m_actionId) : QKeySequence();
    if (keySequence() == held) return;
    const QSignalBlocker block(this);
    setKeySequence(held);
}

void HotkeyBindingEdit::applyEdit() {
    if (!m_manager) return;
    const QKeySequence typed = keySequence();
    // A refusal is reported by the manager's bindingFailed signal; this only
    // has to stop showing a shortcut that is not in effect.
    if (typed != m_manager->binding(m_actionId))
        m_manager->setBinding(m_actionId, typed);
    showHeldBinding();
}

void HotkeyBindingEdit::showEvent(QShowEvent* event) {
    QKeySequenceEdit::showEvent(event);
    // The binding can change while the page is hidden, from Edit > Hotkeys.
    showHeldBinding();
}
