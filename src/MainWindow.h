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
class QAction;
class QDockWidget;
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

    SceneCollection*   m_scenes            = nullptr;
    QUndoStack*        m_undoStack         = nullptr;
    CaptureController* m_captureController = nullptr;
    AudioController*   m_audio             = nullptr;
    MediaController*   m_media             = nullptr;
    HotkeyManager*     m_hotkeys           = nullptr;
    PreviewWidget*     m_preview           = nullptr;  // Program (TimedFrameSource)
    PreviewWidget*     m_stagePreview      = nullptr;  // Staged (studio mode only)
    QPushButton*       m_transitionBtn     = nullptr;  // between staged and program
    QWidget*           m_studioContainer   = nullptr;  // central widget holder
    ScenesPanel*       m_scenesPanel       = nullptr;
    SourcesPanel*      m_sourcesPanel      = nullptr;
    InspectorPanel*    m_inspectorPanel    = nullptr;
    AudioMixerPanel*   m_mixerPanel        = nullptr;
    QDockWidget*       m_scenesDock        = nullptr;
    QDockWidget*       m_sourcesDock       = nullptr;
    QDockWidget*       m_inspectorDock     = nullptr;
    QDockWidget*       m_mixerDock         = nullptr;
    ControlsBar*       m_controlsBar       = nullptr;
    QAction*           m_studioModeAction  = nullptr;
    QImage             m_pendingTransFrom;
    QString            m_captureStatus;
    QString            m_projectPath;
    QAction*           m_saveAction        = nullptr;
    OutputSettings     m_outputSettings;
    StreamSettings     m_streamSettings;
};
