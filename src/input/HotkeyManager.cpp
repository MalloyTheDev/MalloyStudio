#include "HotkeyManager.h"

#include <QCoreApplication>
#include <QSettings>

#include <algorithm>

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
UINT qtKeyToVk(int key, Qt::KeyboardModifiers mods) {
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12)
        return VK_F1 + (key - Qt::Key_F1);

    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return 'A' + (key - Qt::Key_A);

    // Qt reports a numpad digit as the same key as the main-row digit and
    // marks it with KeypadModifier; Windows gives the two different codes.
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return (mods & Qt::KeypadModifier) ? VK_NUMPAD0 + (key - Qt::Key_0)
                                           : '0' + (key - Qt::Key_0);

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

// The combination Windows would be asked for, or {0, 0} when there is none.
// Two sequences Qt tells apart can still be one Win32 hotkey (Return and
// Enter both become VK_RETURN), so duplicates are judged on this.
QPair<UINT, UINT> win32Combination(const QKeySequence& seq) {
    Qt::KeyboardModifiers mods;
    int qtKey = 0;
    if (!decomposeKey(seq, mods, qtKey)) return {0, 0};
    const UINT vk = qtKeyToVk(qtKey, mods);
    if (vk == 0) return {0, 0};
    return {qtModsToWin32(mods), vk};
}

HotkeyManager::Registrar win32Registrar() {
    return {
        [](int id, unsigned modifiers, unsigned virtualKey) {
            return ::RegisterHotKey(nullptr, id, modifiers, virtualKey) != 0;
        },
        [](int id) { ::UnregisterHotKey(nullptr, id); },
    };
}

} // namespace

// ── HotkeyManager ─────────────────────────────────────────────────────────

HotkeyManager::HotkeyManager(QObject* parent) : HotkeyManager(win32Registrar(), parent) {}

HotkeyManager::HotkeyManager(Registrar registrar, QObject* parent)
    : QObject(parent), m_registrar(std::move(registrar)) {
    QCoreApplication::instance()->installNativeEventFilter(this);
}

HotkeyManager::~HotkeyManager() {
    // Unregister all hotkeys.
    for (auto& entry : m_entries) {
        if (entry.registeredId > 0)
            m_registrar.unregisterKey(entry.registeredId);
    }
    QCoreApplication::instance()->removeNativeEventFilter(this);
}

bool HotkeyManager::registerHotkey(int id, const QKeySequence& key) {
    const QPair<UINT, UINT> combination = win32Combination(key);
    if (combination.second == 0) return false;
    return m_registrar.registerKey(id, combination.first, combination.second);
}

void HotkeyManager::unregisterById(int registeredId) {
    if (registeredId > 0)
        m_registrar.unregisterKey(registeredId);
}

void HotkeyManager::ensureEntry(const QString& actionId) {
    if (m_entries.contains(actionId)) return;
    m_entries[actionId] = {actionId, {}, -1};
    m_insertionOrder.append(actionId);
}

bool HotkeyManager::setBinding(const QString& actionId, const QKeySequence& key) {
    return applyBindings({{actionId, key}}).isEmpty();
}

QList<HotkeyManager::Refusal> HotkeyManager::applyBindings(
        const QHash<QString, QKeySequence>& bindings) {
    // Actions new to the manager are added in a fixed order, so registration
    // order does not depend on hash iteration.
    QStringList requested = bindings.keys();
    std::sort(requested.begin(), requested.end());
    for (const QString& id : std::as_const(requested)) ensureEntry(id);

    QStringList ordered;
    for (const QString& id : std::as_const(m_insertionOrder))
        if (bindings.contains(id)) ordered << id;

    auto finalKey = [&](const QString& id) {
        return bindings.contains(id) ? bindings.value(id) : m_entries.value(id).key;
    };

    // Two of our own actions on one shortcut is refused before anything is
    // touched, and reported against the action that has (or would have) it.
    // Windows cannot be relied on to catch it: RegisterHotKey is documented to
    // refuse the second registration, but Windows 11 lets one process register
    // a combination twice, and one key press cannot mean two actions.
    QList<Refusal> refusals;
    QList<QPair<UINT, UINT>> reported;
    for (const QString& id : std::as_const(ordered)) {
        const QKeySequence key = bindings.value(id);
        const QPair<UINT, UINT> combination = win32Combination(key);
        if (combination.second == 0 || reported.contains(combination)) continue;
        for (const QString& other : std::as_const(m_insertionOrder)) {
            if (other == id || win32Combination(finalKey(other)) != combination) continue;
            refusals.append({id, key, other});
            reported.append(combination);
            break;
        }
    }
    if (!refusals.isEmpty()) {
        for (const Refusal& r : std::as_const(refusals))
            emit bindingFailed(r.actionId, r.key, r.heldBy);
        return refusals;
    }

    QStringList changed;
    for (const QString& id : std::as_const(ordered))
        if (m_entries.value(id).key != bindings.value(id)) changed << id;

    // Free every shortcut that is about to move before claiming any.
    QHash<QString, QKeySequence> previous;
    for (const QString& id : std::as_const(changed)) {
        Entry& entry = m_entries[id];
        previous.insert(id, entry.key);
        if (entry.registeredId > 0) {
            unregisterById(entry.registeredId);
            m_idToAction.remove(entry.registeredId);
            entry.registeredId = -1;
        }
    }

    // An empty key means "unbound", which always succeeds. Every refusal is
    // collected rather than stopping at the first, so all of them are reported.
    for (const QString& id : std::as_const(changed)) {
        const QKeySequence key = bindings.value(id);
        if (key.isEmpty()) continue;
        const int newId = m_nextId++;
        if (registerHotkey(newId, key)) {
            m_entries[id].registeredId = newId;
            m_idToAction[newId] = id;
        } else {
            refusals.append({id, key, QString()});
        }
    }

    if (!refusals.isEmpty()) {
        // Windows owns a shortcut elsewhere. Put every action back as it was
        // instead of storing a binding that cannot fire, and say so. The new
        // registrations are released first: in a swap they hold the very keys
        // being restored.
        for (const QString& id : std::as_const(changed)) {
            Entry& entry = m_entries[id];
            if (entry.registeredId > 0) {
                unregisterById(entry.registeredId);
                m_idToAction.remove(entry.registeredId);
                entry.registeredId = -1;
            }
        }
        for (const QString& id : std::as_const(changed)) {
            Entry& entry = m_entries[id];
            const QKeySequence old = previous.value(id);
            entry.key = QKeySequence();
            if (old.isEmpty()) continue;
            const int restoreId = m_nextId++;
            if (registerHotkey(restoreId, old)) {
                entry.key = old;
                entry.registeredId = restoreId;
                m_idToAction[restoreId] = id;
            } else {
                // Lost in the meantime: say so rather than leave it looking bound.
                emit bindingFailed(id, old, QString());
            }
        }
        for (const Refusal& r : std::as_const(refusals))
            emit bindingFailed(r.actionId, r.key, r.heldBy);
        return refusals;
    }

    // Persist.
    QSettings settings;
    for (const QString& id : std::as_const(ordered)) {
        m_entries[id].key = bindings.value(id);
        settings.setValue(QStringLiteral("hotkeys/%1").arg(id), bindings.value(id).toString());
    }
    return {};
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
