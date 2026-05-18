#pragma once
#include <QDialog>
#include <QHash>
#include <QKeySequence>
#include <QString>

class HotkeyManager;
class QTreeWidget;
class QTreeWidgetItem;
class QPushButton;

class HotkeysDialog : public QDialog {
    Q_OBJECT
public:
    explicit HotkeysDialog(HotkeyManager* manager, QWidget* parent = nullptr);

private:
    void buildTree();
    void onRecordClicked(QTreeWidgetItem* item);
    void onClearClicked(QTreeWidgetItem* item);
    void applyAndSave();

    HotkeyManager*  m_manager = nullptr;
    QTreeWidget*    m_tree    = nullptr;

    // Pending changes: actionId → new key sequence (may be empty = clear)
    QHash<QString, QKeySequence> m_pending;

    // Display-name for each built-in action
    static QString displayName(const QString& actionId);
};
