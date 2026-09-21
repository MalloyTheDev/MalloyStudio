#include "HotkeysDialog.h"
#include <QMessageBox>
#include "audio/AudioController.h"
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

QString HotkeysDialog::displayName(const QString& actionId) const {
    if (actionId == QLatin1String(HotkeyManager::kRecordToggle))    return tr("Toggle Recording");
    if (actionId == QLatin1String(HotkeyManager::kStreamToggle))    return tr("Toggle Streaming");
    if (actionId == QLatin1String(HotkeyManager::kReplaySave))      return tr("Save Replay Buffer");
    if (actionId == QLatin1String(HotkeyManager::kStudioTransition)) return tr("Studio Transition");
    if (actionId.startsWith(QStringLiteral("scene.switch."))) {
        const int n = actionId.mid(13).toInt();
        return tr("Switch to Scene %1").arg(n);
    }
    if (actionId.startsWith(QStringLiteral("audio.mute."))) {
        // Resolve the human-friendly device name via AudioController if we
        // have one; fall back to the raw input id for tests that pass nullptr.
        const QString id = actionId.mid(11);
        if (m_audio) {
            for (const AudioInput& in : m_audio->inputs()) {
                if (in.id == id) return tr("Mute: %1").arg(in.name);
            }
        }
        return tr("Mute: %1").arg(id);
    }
    return actionId;
}

// ── Dialog ────────────────────────────────────────────────────────────────

HotkeysDialog::HotkeysDialog(HotkeyManager* manager, AudioController* audio, QWidget* parent)
    : QDialog(parent), m_manager(manager), m_audio(audio)
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
        // A shortcut that was refused is not in effect, and saying nothing
        // would leave the user believing it was.
        const QString refused = applyPending();
        if (!refused.isEmpty())
            QMessageBox::warning(this, tr("Shortcut not available"), refused);
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

    // v7 Tier 4: per-source mute actions. We enumerate every AudioController
    // input (loopback + per-source mics) and emit an `audio.mute.<id>` row.
    // Action IDs are stable as long as the device GUID is, so bindings
    // survive an unplug/replug of the device.
    if (m_audio) {
        for (const AudioInput& in : m_audio->inputs()) {
            actionIds << QStringLiteral("audio.mute.") + in.id;
        }
    }

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

QString HotkeysDialog::applyPending() {
    const QList<HotkeyManager::Refusal> refusals = m_manager->applyBindings(m_pending);
    m_pending.clear();
    if (refusals.isEmpty()) return QString();

    // Only a shortcut Windows refused can be blamed on another application.
    // One that another action here holds is named with that action, since
    // moving it is what the user has to do.
    QStringList taken;
    QStringList refused;
    for (const HotkeyManager::Refusal& r : refusals) {
        const QString key = r.key.toString(QKeySequence::NativeText);
        if (r.heldBy.isEmpty())
            refused << QStringLiteral("%1  (%2)").arg(displayName(r.actionId), key);
        else
            taken << tr("%1  (%2 is already used by %3)")
                         .arg(displayName(r.actionId), key, displayName(r.heldBy));
    }

    QStringList parts;
    if (!taken.isEmpty())
        parts << tr("These shortcuts are already assigned to another action:\n\n%1")
                     .arg(taken.join(QStringLiteral("\n")));
    if (!refused.isEmpty())
        parts << tr("These shortcuts could not be registered:\n\n%1\n\n"
                    "Another application may already claim them, or the combination "
                    "may not be supported.")
                     .arg(refused.join(QStringLiteral("\n")));
    parts << tr("None of the edits was applied.");
    return parts.join(QStringLiteral("\n\n"));
}

// MOC for KeyCaptureEdit (Q_OBJECT defined in this .cpp file).
#include "HotkeysDialog.moc"
