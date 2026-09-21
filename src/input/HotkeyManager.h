#pragma once
#include <QAbstractNativeEventFilter>
#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>

// HotkeyManager registers system-wide (process-level) hotkeys via Win32
// RegisterHotKey and dispatches triggered() when the user presses them,
// even if MalloyStudio doesn't have focus.
//
// Usage:
//   auto* hk = new HotkeyManager(this);
//   hk->setBinding("record.toggle", QKeySequence("Ctrl+Shift+R"));
//   connect(hk, &HotkeyManager::triggered, ...);
//
// Bindings are persisted in QSettings under "hotkeys/<actionId>".
//
// Supported key components: F1–F12, A–Z, 0–9 with any combination of
// Ctrl / Shift / Alt / Win modifiers. A digit carrying Qt::KeypadModifier is
// the numpad key of that digit, which Windows treats as a different key.

class HotkeyManager : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
public:
    // Predefined action IDs (add more as needed).
    static constexpr const char* kRecordToggle    = "record.toggle";
    static constexpr const char* kStreamToggle    = "stream.toggle";
    static constexpr const char* kReplaySave       = "replay.save";
    static constexpr const char* kStudioTransition = "studio.transition";
    // Scene switch: "scene.switch.1" … "scene.switch.9"
    static QString sceneActionId(int oneBased) {
        return QStringLiteral("scene.switch.%1").arg(oneBased);
    }

    // The two calls that reach Windows. Tests substitute their own so bindings
    // can be exercised without claiming system-wide shortcuts. `modifiers` are
    // Win32 MOD_* flags and `virtualKey` a VK_* code.
    struct Registrar {
        std::function<bool(int id, unsigned modifiers, unsigned virtualKey)> registerKey;
        std::function<void(int id)> unregisterKey;
    };

    // A binding that was not made. heldBy names another of this manager's
    // actions that has, or would be given, the same shortcut. It is empty when
    // Windows refused the shortcut: another application owns it, or the
    // combination is not supported.
    struct Refusal {
        QString      actionId;
        QKeySequence key;
        QString      heldBy;
    };

    explicit HotkeyManager(QObject* parent = nullptr);
    HotkeyManager(Registrar registrar, QObject* parent = nullptr);
    ~HotkeyManager() override;

    // Set or change the hotkey for an action. Pass an empty QKeySequence to
    // unregister. Automatically unregisters any previous binding for the
    // same actionId and saves to QSettings.
    // Returns false when the shortcut was refused, either by Windows or
    // because another action already has it. On failure the previous binding
    // is restored if it can be, nothing is persisted, and bindingFailed() is
    // emitted: a shortcut that cannot be registered must not be shown as if it
    // works.
    bool setBinding(const QString& actionId, const QKeySequence& key);

    // Applies several bindings as one change, so shortcuts can be swapped or
    // moved between actions. Every changed action is unregistered before any
    // new shortcut is registered, because RegisterHotKey is documented to
    // refuse a combination that is still registered, and in a swap each new
    // shortcut is still held by the other action. It is all or nothing: two
    // actions ending up with the same shortcut is refused before anything
    // changes, and if Windows refuses any shortcut every action is put back as
    // it was. Returns what was refused, empty when everything was applied and
    // saved.
    QList<Refusal> applyBindings(const QHash<QString, QKeySequence>& bindings);

    // Return the current binding for an action (empty if none).
    QKeySequence binding(const QString& actionId) const;

    // Load all persisted bindings from QSettings and register them.
    void loadBindings();

    // Registered action IDs (in insertion order).
    QList<QString> actionIds() const { return m_insertionOrder; }

    // QAbstractNativeEventFilter
    bool nativeEventFilter(const QByteArray& eventType,
                           void* message, qintptr* result) override;

signals:
    void triggered(const QString& actionId);
    // A shortcut was refused, with the action that holds it when that is one
    // of ours (see Refusal). Emitted from setBinding() and applyBindings(), and
    // from loadBindings() at startup, so a binding that stopped working since
    // it was saved is surfaced rather than failing quietly every launch.
    void bindingFailed(const QString& actionId, const QKeySequence& key,
                       const QString& heldBy);

private:
    void unregisterById(int registeredId);
    bool registerHotkey(int id, const QKeySequence& key);
    void ensureEntry(const QString& actionId);

    struct Entry {
        QString      actionId;
        QKeySequence key;
        int          registeredId = -1; // >0 when actively registered
    };

    Registrar             m_registrar;
    QHash<QString, Entry> m_entries;        // actionId → Entry
    QHash<int, QString>   m_idToAction;     // Win32 hotkey id → actionId
    QList<QString>        m_insertionOrder; // for deterministic iteration
    int                   m_nextId = 1;
};
