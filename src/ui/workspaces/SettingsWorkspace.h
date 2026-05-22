#pragma once

#include <QKeySequence>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QListWidget;
class QStackedWidget;

// Settings workspace (secondary.jsx SettingsScreen): a two-pane screen with a
// group list on the left and a content area on the right. The Recording group
// is built out in full; other groups share a generic same-pattern page. This
// is the surface that will eventually consolidate the standalone dialogs.
class SettingsWorkspace : public QWidget {
    Q_OBJECT
public:
    explicit SettingsWorkspace(QWidget* parent = nullptr);

signals:
    // Emitted when the user applies Recording settings (persisted to
    // OutputSettings); MainWindow reloads and re-applies live state.
    void recordingSettingsApplied();
    // Master-bus limiter changed in Settings ▸ Audio. MainWindow applies it to
    // the live AudioController. thresholdDb is in dBFS (e.g. -3.0).
    void audioLimiterChanged(bool enabled, double thresholdDb);
    // A global hotkey binding changed in Settings ▸ Hotkeys. MainWindow forwards
    // it to HotkeyManager::setBinding (which persists + re-registers it).
    void hotkeyChanged(const QString& actionId, const QKeySequence& seq);

protected:
    void showEvent(QShowEvent* event) override;   // focus the section list on entry

private:
    QWidget* buildRecordingPage();
    QWidget* buildGeneralPage();
    QWidget* buildStreamingPage();
    QWidget* buildVideoPage();
    QWidget* buildAudioPage();
    QWidget* buildStoragePage();
    QWidget* buildHotkeysPage();
    QWidget* buildPerformancePage();
    QWidget* buildAppearancePage();
    QWidget* buildAccountsPage();
    QWidget* buildExperimentalPage();
    QWidget* buildAIPage();
    void loadRecordingSettings();
    void applyRecordingSettings();
    void updateEncoderDerived();   // rate-control/CRF follow the chosen encoder

    QStackedWidget* m_stack = nullptr;
    QListWidget* m_nav = nullptr;
    QComboBox* m_encoderCombo = nullptr;
    QComboBox* m_rateCombo = nullptr;
    QComboBox* m_resCombo = nullptr;
    QComboBox* m_fpsCombo = nullptr;
    QComboBox* m_containerCombo = nullptr;
    QLineEdit* m_crfEdit = nullptr;
    QCheckBox* m_replayCheck = nullptr;
    QLineEdit* m_folderEdit = nullptr;     // recording output dir (recording/lastDir)
    QLineEdit* m_filenameEdit = nullptr;   // filename pattern (recording/filenamePattern)
    QCheckBox* m_autoStart = nullptr;
    QCheckBox* m_autoStop = nullptr;
    QCheckBox* m_saveClip = nullptr;
};
