#pragma once

#include <QKeySequenceEdit>
#include <QPointer>
#include <QString>

class HotkeyManager;

// The shortcut field for one action on Settings > Hotkeys. It shows the
// binding the HotkeyManager holds rather than what was last typed: a shortcut
// the manager refuses is not in effect, so after a refusal the field goes back
// to the one that is. Disabled until a manager is attached.
class HotkeyBindingEdit : public QKeySequenceEdit {
    Q_OBJECT
public:
    explicit HotkeyBindingEdit(const QString& actionId, QWidget* parent = nullptr);

    QString actionId() const { return m_actionId; }

    void setManager(HotkeyManager* manager);

    // Shows what the manager holds now, applying nothing.
    void showHeldBinding();

protected:
    void showEvent(QShowEvent* event) override;

private:
    void applyEdit();

    QString                m_actionId;
    QPointer<HotkeyManager> m_manager;
};
