#pragma once
#include <QAbstractNativeEventFilter>
#include <QHash>
#include <QKeySequence>
#include <QObject>
#include <QString>

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
// Ctrl / Shift / Alt / Win modifiers.

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

    explicit HotkeyManager(QObject* parent = nullptr);
    ~HotkeyManager() override;

    // Set or change the hotkey for an action. Pass an empty QKeySequence to
    // unregister. Automatically unregisters any previous binding for the
    // same actionId and saves to QSettings.
    void setBinding(const QString& actionId, const QKeySequence& key);

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

private:
    void unregisterById(int registeredId);
    bool registerHotkey(int id, const QKeySequence& key);

    struct Entry {
        QString      actionId;
        QKeySequence key;
        int          registeredId = -1; // >0 when actively registered
    };

    QHash<QString, Entry> m_entries;        // actionId → Entry
    QHash<int, QString>   m_idToAction;     // Win32 hotkey id → actionId
    QList<QString>        m_insertionOrder; // for deterministic iteration
    int                   m_nextId = 1;
};
