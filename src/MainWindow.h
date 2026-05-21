#pragma once
#include "recording/OutputSettings.h"
#include "recording/StreamSettings.h"

#include <QImage>
#include <QMainWindow>
#include <QString>

class SceneCollection;
class ControlsBar;
class HotkeyManager;
class PreviewWidget;
class ScenesPanel;
class SourcesPanel;
class InspectorPanel;
class AudioMixerPanel;
class AudioController;
class MediaController;
class CaptureController;
class AppShell;
class Dashboard;
class StreamingWorkspace;
class SettingsWorkspace;
class CommandPalette;
class OnboardingOverlay;
class QAction;
class QPushButton;
class QUndoStack;
class QWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void setupUi();
    void setupMenus();
    void connectModelSignals();
    bool maybeSave();
    void newProject();
    void openProject();
    bool saveProject();
    bool saveProjectAs();
    bool loadProject(const QString& filePath);
    void updateWindowTitle();
    void updateStatusBar();
    void updatePreviewLabel();
    void updateShellMode();              // reflect record/stream state in the status bar
    void flash(const QString& text, int ms = 4000);  // transient status toast

    SceneCollection*   m_scenes            = nullptr;
    QUndoStack*        m_undoStack         = nullptr;
    CaptureController* m_captureController = nullptr;
    AudioController*   m_audio             = nullptr;
    MediaController*   m_media             = nullptr;
    HotkeyManager*     m_hotkeys           = nullptr;
    AppShell*          m_shell             = nullptr;  // icon rail + workspaces + status bar
    Dashboard*         m_dashboard         = nullptr;
    StreamingWorkspace* m_streamStudio     = nullptr;
    SettingsWorkspace* m_settings          = nullptr;
    CommandPalette*    m_palette           = nullptr;
    OnboardingOverlay* m_onboarding        = nullptr;
    PreviewWidget*     m_preview           = nullptr;  // Program (TimedFrameSource)
    PreviewWidget*     m_stagePreview      = nullptr;  // Staged (studio mode only)
    QPushButton*       m_transitionBtn     = nullptr;  // between staged and program
    QWidget*           m_studioContainer   = nullptr;  // preview area holder
    ScenesPanel*       m_scenesPanel       = nullptr;
    SourcesPanel*      m_sourcesPanel      = nullptr;
    InspectorPanel*    m_inspectorPanel    = nullptr;
    AudioMixerPanel*   m_mixerPanel        = nullptr;
    ControlsBar*       m_controlsBar       = nullptr;
    QAction*           m_studioModeAction  = nullptr;
    QImage             m_pendingTransFrom;
    QString            m_captureStatus;
    QString            m_projectPath;
    QAction*           m_saveAction        = nullptr;
    OutputSettings     m_outputSettings;
    StreamSettings     m_streamSettings;
};
