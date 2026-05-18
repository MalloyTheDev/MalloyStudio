#include "HotkeysDialog.h"
#include "input/HotkeyManager.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

// ── Key-capture line edit ─────────────────────────────────────────────────
// Defined at file scope so MOC can process its Q_OBJECT.

class KeyCaptureEdit : public QLineEdit {
    Q_OBJECT
public:
    explicit KeyCaptureEdit(QWidget* parent = nullptr) : QLineEdit(parent) {
        setReadOnly(true);
        setPlaceholderText(tr("Press a key combination…"));
    }

    QKeySequence captured() const { return m_captured; }

    void clearCapture() {
        m_captured = {};
        QLineEdit::clear();
        emit captureChanged(m_captured);
    }

signals:
    void captureChanged(const QKeySequence& seq);

protected:
    void keyPressEvent(QKeyEvent* event) override {
        const Qt::Key key = static_cast<Qt::Key>(event->key());
        // Ignore bare modifier presses.
        if (key == Qt::Key_unknown || key == Qt::Key_Control ||
            key == Qt::Key_Shift   || key == Qt::Key_Alt     || key == Qt::Key_Meta)
            return;
        // Escape → clear
        if (key == Qt::Key_Escape) {
            clearCapture();
            return;
        }
        const QKeyCombination combo(event->modifiers(), key);
        m_captured = QKeySequence(combo);
        setText(m_captured.toString(QKeySequence::NativeText));
        emit captureChanged(m_captured);
    }

private:
    QKeySequence m_captured;
};

// ── Static helpers ────────────────────────────────────────────────────────

QString HotkeysDialog::displayName(const QString& actionId) {
    if (actionId == QLatin1String(HotkeyManager::kRecordToggle))    return tr("Toggle Recording");
    if (actionId == QLatin1String(HotkeyManager::kStreamToggle))    return tr("Toggle Streaming");
    if (actionId == QLatin1String(HotkeyManager::kReplaySave))      return tr("Save Replay Buffer");
    if (actionId == QLatin1String(HotkeyManager::kStudioTransition)) return tr("Studio Transition");
    if (actionId.startsWith(QStringLiteral("scene.switch."))) {
        const int n = actionId.mid(13).toInt();
        return tr("Switch to Scene %1").arg(n);
    }
    return actionId;
}

// ── Dialog ────────────────────────────────────────────────────────────────

HotkeysDialog::HotkeysDialog(HotkeyManager* manager, QWidget* parent)
    : QDialog(parent), m_manager(manager)
{
    setWindowTitle(tr("Hotkeys"));
    setMinimumSize(480, 420);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({tr("Action"), tr("Hotkey")});
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->setRootIsDecorated(false);
    m_tree->setAlternatingRowColors(true);

    buildTree();

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        applyAndSave();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* root = new QVBoxLayout(this);
    root->addWidget(new QLabel(
        tr("Click the hotkey field and press a key combination to bind it.\n"
           "Press Escape in the field to clear a binding."), this));
    root->addWidget(m_tree, 1);
    root->addWidget(buttons);
}

void HotkeysDialog::buildTree() {
    // Collect well-known built-in actions in a fixed order.
    QStringList actionIds;
    actionIds << HotkeyManager::kRecordToggle
              << HotkeyManager::kStreamToggle
              << HotkeyManager::kReplaySave
              << HotkeyManager::kStudioTransition;
    for (int i = 1; i <= 9; ++i)
        actionIds << HotkeyManager::sceneActionId(i);

    // Append any extra ids already registered in the manager.
    for (const QString& id : m_manager->actionIds()) {
        if (!actionIds.contains(id))
            actionIds << id;
    }

    for (const QString& actionId : actionIds) {
        const QKeySequence current = m_manager->binding(actionId);

        auto* item = new QTreeWidgetItem(m_tree);
        item->setText(0, displayName(actionId));
        item->setData(0, Qt::UserRole, actionId);

        auto* captureEdit = new KeyCaptureEdit(m_tree);
        if (!current.isEmpty())
            captureEdit->setText(current.toString(QKeySequence::NativeText));
        m_tree->setItemWidget(item, 1, captureEdit);

        connect(captureEdit, &KeyCaptureEdit::captureChanged, this,
                [this, actionId](const QKeySequence& seq) {
            m_pending[actionId] = seq;
        });
    }
}

void HotkeysDialog::applyAndSave() {
    for (auto it = m_pending.constBegin(); it != m_pending.constEnd(); ++it)
        m_manager->setBinding(it.key(), it.value());
    m_pending.clear();
}

// MOC for KeyCaptureEdit (Q_OBJECT defined in this .cpp file).
#include "HotkeysDialog.moc"
