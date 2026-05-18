#include "HotkeyManager.h"

#include <QCoreApplication>
#include <QSettings>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// ── Key-sequence ↔ Win32 helpers ──────────────────────────────────────────

namespace {

// Convert a Qt modifier set to Win32 MOD_* flags.
UINT qtModsToWin32(Qt::KeyboardModifiers mods) {
    UINT flags = MOD_NOREPEAT;
    if (mods & Qt::ControlModifier) flags |= MOD_CONTROL;
    if (mods & Qt::ShiftModifier)   flags |= MOD_SHIFT;
    if (mods & Qt::AltModifier)     flags |= MOD_ALT;
    if (mods & Qt::MetaModifier)    flags |= MOD_WIN;
    return flags;
}

// Convert a Qt key value to a Win32 virtual-key code.
// Returns 0 if the key is not mappable.
UINT qtKeyToVk(int key) {
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12)
        return VK_F1 + (key - Qt::Key_F1);

    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return 'A' + (key - Qt::Key_A);

    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return '0' + (key - Qt::Key_0);

    switch (key) {
        case Qt::Key_Space:     return VK_SPACE;
        case Qt::Key_Return:    return VK_RETURN;
        case Qt::Key_Enter:     return VK_RETURN;
        case Qt::Key_Backspace: return VK_BACK;
        case Qt::Key_Delete:    return VK_DELETE;
        case Qt::Key_Insert:    return VK_INSERT;
        case Qt::Key_Home:      return VK_HOME;
        case Qt::Key_End:       return VK_END;
        case Qt::Key_PageUp:    return VK_PRIOR;
        case Qt::Key_PageDown:  return VK_NEXT;
        case Qt::Key_Up:        return VK_UP;
        case Qt::Key_Down:      return VK_DOWN;
        case Qt::Key_Left:      return VK_LEFT;
        case Qt::Key_Right:     return VK_RIGHT;
        case Qt::Key_Escape:    return VK_ESCAPE;
        case Qt::Key_Tab:       return VK_TAB;
        default:                return 0;
    }
}

// Split a QKeySequence (first binding) into key + mods, return true if valid.
bool decomposeKey(const QKeySequence& seq, Qt::KeyboardModifiers& mods, int& key) {
    if (seq.isEmpty()) return false;
    const Qt::KeyboardModifiers m = seq[0].keyboardModifiers();
    const Qt::Key               k = seq[0].key();
    if (k == Qt::Key_unknown || k == Qt::Key_Control ||
        k == Qt::Key_Shift   || k == Qt::Key_Alt     || k == Qt::Key_Meta)
        return false;
    mods = m;
    key  = static_cast<int>(k);
    return true;
}

} // namespace

// ── HotkeyManager ─────────────────────────────────────────────────────────

HotkeyManager::HotkeyManager(QObject* parent) : QObject(parent) {
    QCoreApplication::instance()->installNativeEventFilter(this);
}

HotkeyManager::~HotkeyManager() {
    // Unregister all hotkeys.
    for (auto& entry : m_entries) {
        if (entry.registeredId > 0)
            UnregisterHotKey(nullptr, entry.registeredId);
    }
    QCoreApplication::instance()->removeNativeEventFilter(this);
}

bool HotkeyManager::registerHotkey(int id, const QKeySequence& key) {
    Qt::KeyboardModifiers mods;
    int qtKey = 0;
    if (!decomposeKey(key, mods, qtKey)) return false;

    const UINT vk = qtKeyToVk(qtKey);
    if (vk == 0) return false;

    return ::RegisterHotKey(nullptr, id, qtModsToWin32(mods), vk) != 0;
}

void HotkeyManager::unregisterById(int registeredId) {
    if (registeredId > 0)
        ::UnregisterHotKey(nullptr, registeredId);
}

void HotkeyManager::setBinding(const QString& actionId, const QKeySequence& key) {
    // Ensure entry exists.
    if (!m_entries.contains(actionId)) {
        m_entries[actionId] = {actionId, {}, -1};
        m_insertionOrder.append(actionId);
    }
    Entry& entry = m_entries[actionId];

    // Unregister old binding.
    if (entry.registeredId > 0) {
        unregisterById(entry.registeredId);
        m_idToAction.remove(entry.registeredId);
        entry.registeredId = -1;
    }

    entry.key = key;

    // Register new binding.
    if (!key.isEmpty()) {
        const int newId = m_nextId++;
        if (registerHotkey(newId, key)) {
            entry.registeredId = newId;
            m_idToAction[newId] = actionId;
        }
    }

    // Persist.
    QSettings settings;
    settings.setValue(QStringLiteral("hotkeys/%1").arg(actionId), key.toString());
}

QKeySequence HotkeyManager::binding(const QString& actionId) const {
    auto it = m_entries.constFind(actionId);
    return (it != m_entries.constEnd()) ? it->key : QKeySequence{};
}

void HotkeyManager::loadBindings() {
    QSettings settings;
    settings.beginGroup(QStringLiteral("hotkeys"));
    const QStringList keys = settings.childKeys();
    for (const QString& k : keys) {
        const QString seq = settings.value(k).toString();
        if (!seq.isEmpty())
            setBinding(k, QKeySequence(seq));
    }
    settings.endGroup();
}

bool HotkeyManager::nativeEventFilter(const QByteArray& eventType,
                                      void* message, qintptr* /*result*/) {
#ifdef Q_OS_WIN
    if (eventType == "windows_generic_MSG" || eventType == "windows_dispatcher_MSG") {
        const MSG* msg = static_cast<const MSG*>(message);
        if (msg->message == WM_HOTKEY) {
            const int id = static_cast<int>(msg->wParam);
            auto it = m_idToAction.constFind(id);
            if (it != m_idToAction.constEnd()) {
                emit triggered(it.value());
                return true; // consume the event
            }
        }
    }
#else
    Q_UNUSED(eventType); Q_UNUSED(message);
#endif
    return false;
}
