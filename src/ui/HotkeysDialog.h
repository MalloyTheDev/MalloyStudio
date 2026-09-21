#pragma once
#include <QDialog>
#include <QHash>
#include <QKeySequence>
#include <QString>

class AudioController;
class HotkeyManager;
class QTreeWidget;
class QTreeWidgetItem;
class QPushButton;

class HotkeysDialog : public QDialog {
    Q_OBJECT
public:
    // AudioController is optional; pass nullptr to skip the per-mic Mute group.
    explicit HotkeysDialog(HotkeyManager* manager,
                           AudioController* audio = nullptr,
                           QWidget* parent = nullptr);

    // Applies the shortcuts edited so far as one change, so they can be
    // swapped or moved between actions, and returns a description of any
    // that were refused. Empty when every one took effect. OK calls this and
    // shows the description.
    QString applyPending();

private:
    void buildTree();
    void onRecordClicked(QTreeWidgetItem* item);
    void onClearClicked(QTreeWidgetItem* item);

    HotkeyManager*   m_manager = nullptr;
    AudioController* m_audio   = nullptr;
    QTreeWidget*     m_tree    = nullptr;

    // Pending changes: actionId → new key sequence (may be empty = clear)
    QHash<QString, QKeySequence> m_pending;

    // Display-name for each built-in action
    QString displayName(const QString& actionId) const;
};
