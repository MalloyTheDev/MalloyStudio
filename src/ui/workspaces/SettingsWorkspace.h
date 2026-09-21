#pragma once

#include <QList>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QListWidget;
class QStackedWidget;
class QLabel;
class QPushButton;
class TwitchAuth;
class TwitchApi;
class AudioController;
class HotkeyBindingEdit;
class HotkeyManager;

// Settings workspace (secondary.jsx SettingsScreen): a two-pane screen with a
// group list on the left and a content area on the right. The Recording group
// is built out in full; other groups share a generic same-pattern page. This
// is the surface that will eventually consolidate the standalone dialogs.
class SettingsWorkspace : public QWidget {
    Q_OBJECT
public:
    explicit SettingsWorkspace(QWidget* parent = nullptr);

    // The application's single TwitchAuth/TwitchApi pair. There must be exactly
    // one of each: Twitch refresh tokens are one time use, so two instances
    // refreshing independently would invalidate each other's stored token.
    void setTwitch(TwitchAuth* auth, TwitchApi* api);

    // The live audio controller, so hardware detection can enumerate capture
    // devices instead of guessing.
    void setAudioController(AudioController* audio) { m_audio = audio; }

    // The live hotkey manager. Settings > Hotkeys edits its bindings directly
    // and shows what it holds, so a refused shortcut is not left on screen.
    void setHotkeyManager(HotkeyManager* hotkeys);

signals:
    // The Twitch account connection changed, or a stream key was fetched.
    // MainWindow reloads StreamSettings so the new key is used.
    void streamCredentialsChanged();
    // Emitted when the user applies Recording settings (persisted to
    // OutputSettings); MainWindow reloads and re-applies live state.
    void recordingSettingsApplied();
    // Master-bus limiter changed in Settings ▸ Audio. MainWindow applies it to
    // the live AudioController. thresholdDb is in dBFS (e.g. -3.0).
    void audioLimiterChanged(bool enabled, double thresholdDb);
    // The capture backend was changed in Settings ▸ Performance. A running
    // session keeps the backend it was started with, so MainWindow restarts
    // capture: the alternative is a setting that appears to have taken effect
    // and has not, which is the worst of the three possible behaviours.
    void captureBackendChanged();

protected:
    void showEvent(QShowEvent* event) override;   // focus the section list on entry

private:
    AudioController* m_audio = nullptr;
    TwitchAuth*  m_twitchAuth = nullptr;
    TwitchApi*   m_twitchApi = nullptr;
    QLabel*      m_twitchStatus = nullptr;
    QPushButton* m_twitchButton = nullptr;

    QWidget* buildRecordingPage();
    QWidget* buildGeneralPage();
    QWidget* buildStreamingPage();
    void beginTwitchConnect();
    void refreshTwitchStatus();
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
    QList<HotkeyBindingEdit*> m_hotkeyEdits;
};
