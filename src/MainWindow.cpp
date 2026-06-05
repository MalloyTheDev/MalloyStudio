#include "MainWindow.h"
#include "ui/ControlsBar.h"
#include "ui/PreviewWidget.h"
#include "ui/ScenesPanel.h"
#include "ui/SourcesPanel.h"
#include "ui/InspectorPanel.h"
#include "ui/AudioMixerPanel.h"
#include "ui/OutputSettingsDialog.h"
#include "ui/StreamSettingsDialog.h"
#include "ui/HotkeysDialog.h"
#include "ui/shell/AppShell.h"
#include "ui/shell/WorkspaceHeader.h"
#include "ui/shell/StudioStatusBar.h"
#include "ui/shell/NavModel.h"
#include "ui/CommandPalette.h"
#include "ui/OnboardingOverlay.h"
#include "ui/workspaces/RecordingWorkspace.h"
#include "ui/workspaces/StreamingWorkspace.h"
#include "ui/workspaces/SettingsWorkspace.h"
#include "ui/workspaces/AILabWorkspace.h"
#include "ui/workspaces/EditorWorkspace.h"
#include "ui/workspaces/LibraryWorkspaces.h"
#include "ui/dashboard/Dashboard.h"
#include "audio/AudioController.h"
#include "input/HotkeyManager.h"
#include "recording/MediaController.h"
#include "model/SceneCollection.h"
#include "model/Scene.h"
#include "capture/CaptureController.h"
#include "project/ProjectDocument.h"
#include "project/ClipsRegistry.h"
#include "project/ProjectRegistry.h"
#include "project/MediaRegistry.h"
#include "recording/RenderQueue.h"

#include <QAction>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QStandardPaths>
#include <QTimer>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    resize(1280, 720);

    m_scenes           = new SceneCollection(this);
    m_undoStack        = new QUndoStack(this);
    m_scenes->setUndoStack(m_undoStack);
    m_captureController = new CaptureController(m_scenes, this);
    m_audio             = new AudioController(this);
    m_clipsRegistry     = new ClipsRegistry(this);
    m_projectRegistry   = new ProjectRegistry(this);
    m_mediaRegistry     = new MediaRegistry(this);
    m_renderQueue       = new RenderQueue(this);
    m_outputSettings    = OutputSettings::load();
    m_streamSettings    = StreamSettings::load();
    m_captureStatus     = m_captureController->statusSummary();

    // MediaController is constructed after PreviewWidget (which implements
    // TimedFrameSource) — created in setupUi(); wire-up happens after.
    setupUi();
    m_media   = new MediaController(m_preview, m_audio, this);
    m_hotkeys = new HotkeyManager(this);
    m_hotkeys->loadBindings();

    setupMenus();
    connectModelSignals();

    // Apply persisted replay buffer duration to both live sources at startup.
    m_preview->setReplayBufferSeconds(m_outputSettings.replayBufferSeconds);
    m_audio->setReplayBufferSeconds(m_outputSettings.replayBufferSeconds);

    QSettings settings;
    restoreGeometry(settings.value("geometry").toByteArray());
    restoreState(settings.value("windowState").toByteArray());

    if (!m_media->ffmpegAvailable()) {
        const QString tip = tr("ffmpeg.exe not found in PATH. Install ffmpeg to enable recording/streaming.");
        m_controlsBar->setRecordEnabled(false, tip);
        m_controlsBar->setStreamEnabled(false, tip);
    }

    updateStatusBar();
    updatePreviewLabel();
    updateWindowTitle();
}

MainWindow::~MainWindow() {
    if (m_media) {
        if (m_media->isRecording()) m_media->stopRecording();
        if (m_media->isStreaming()) m_media->stopStreaming();
    }
    m_captureController->stopAll();
}

void MainWindow::setupUi() {
    // ── Studio mode container ──────────────────────────────────────────────
    // Holds two preview panes side-by-side (Staged | Transition | Program).
    // In single-canvas mode only the Program pane is visible.
    m_studioContainer = new QWidget(this);
    auto* studioLayout = new QHBoxLayout(m_studioContainer);
    studioLayout->setContentsMargins(0, 0, 0, 0);
    studioLayout->setSpacing(4);

    m_stagePreview = new PreviewWidget(m_scenes, PreviewWidget::Role::Staged, m_studioContainer);
    m_stagePreview->setVisible(false);   // hidden until studio mode is on
    studioLayout->addWidget(m_stagePreview, 1);

    // Transition column: a vertical strip with a label + Cut / Fade buttons
    auto* transCol = new QWidget(m_studioContainer);
    transCol->setFixedWidth(80);
    transCol->setVisible(false);
    auto* transColLayout = new QVBoxLayout(transCol);
    transColLayout->setContentsMargins(2, 4, 2, 4);
    transColLayout->setSpacing(4);
    auto* transLabel = new QLabel(tr("Transition"), transCol);
    transLabel->setAlignment(Qt::AlignCenter);
    QFont lf = transLabel->font();
    lf.setPointSize(8);
    transLabel->setFont(lf);
    transColLayout->addWidget(transLabel);
    m_transitionBtn = new QPushButton(tr("▶ Cut"), transCol);
    m_transitionBtn->setToolTip(tr("Promote staged scene to program"));
    transColLayout->addWidget(m_transitionBtn);
    transColLayout->addStretch();
    studioLayout->addWidget(transCol);

    m_preview = new PreviewWidget(m_scenes, PreviewWidget::Role::Program, m_studioContainer);
    studioLayout->addWidget(m_preview, 1);

    // Wire transition column visibility to studioMode toggle
    connect(m_scenes, &SceneCollection::studioModeChanged, this,
            [this, transCol](bool on) {
        m_stagePreview->setVisible(on);
        transCol->setVisible(on);
    });

    // Capture panels (formerly dockable; now arranged as fixed columns inside
    // the Recording workspace).
    m_scenesPanel = new ScenesPanel(m_scenes, this);
    // v7: SourcesPanel needs AudioController so the "Microphone" entry in the
    // Add dialog can launch MicrophonePickerDialog with the live device list.
    m_sourcesPanel = new SourcesPanel(m_scenes, m_audio, this);
    m_inspectorPanel = new InspectorPanel(m_scenes, m_captureController, m_audio, this);
    // v7: pass SceneCollection so the mixer's "+ Add Microphone" button can
    // create a scene-level AudioInput source via addAudioInputToCurrent().
    m_mixerPanel = new AudioMixerPanel(m_audio, m_scenes, this);
    m_controlsBar = new ControlsBar(this);

    // ── App shell: icon rail + workspace stack + status bar ────────────────
    m_shell = new AppShell(this);

    auto* recording = new RecordingWorkspace(
        m_scenesPanel, m_sourcesPanel, m_studioContainer, m_controlsBar,
        m_mixerPanel, m_inspectorPanel);

    m_dashboard = new Dashboard(m_renderQueue, this);
    connect(m_dashboard, &Dashboard::navigateTo, this,
            [this](const QString& id) { m_shell->setCurrentWorkspace(id); });
    connect(m_dashboard, &Dashboard::recordRequested, this, [this] { m_controlsBar->toggleRecord(); });
    connect(m_dashboard, &Dashboard::streamRequested, this, [this] { m_controlsBar->toggleStream(); });
    m_shell->addWorkspace(QStringLiteral("dashboard"), m_dashboard);
    m_shell->addWorkspace(QStringLiteral("record"), recording);
    m_streamStudio = new StreamingWorkspace(m_audio, this);
    connect(m_streamStudio, &StreamingWorkspace::goLiveRequested,
            this, [this] { m_controlsBar->toggleStream(); });
    m_shell->addWorkspace(QStringLiteral("stream"), m_streamStudio);
    m_editor = new EditorWorkspace(m_mediaRegistry, this);
    m_shell->addWorkspace(QStringLiteral("editor"), m_editor);
    m_shell->addWorkspace(QStringLiteral("clips"), new ClipsWorkspace(m_clipsRegistry, this));
    m_shell->addWorkspace(QStringLiteral("media"), new MediaWorkspace(m_mediaRegistry, this));
    auto* projects = new ProjectsWorkspace(m_projectRegistry, this);
    connect(projects, &ProjectsWorkspace::openRequested, this, [this](const QString& path) {
        if (maybeSave()) loadProject(path);
    });
    connect(projects, &ProjectsWorkspace::newRequested, this, &MainWindow::newProject);
    m_shell->addWorkspace(QStringLiteral("projects"), projects);
    m_shell->addWorkspace(QStringLiteral("render"), new RenderWorkspace(m_renderQueue, this));
    connect(m_renderQueue, &RenderQueue::changed, this, [this] { updateShellMode(); });
    m_shell->addWorkspace(QStringLiteral("ai"), new AILabWorkspace(this));
    m_settings = new SettingsWorkspace(this);
    connect(m_settings, &SettingsWorkspace::recordingSettingsApplied, this, [this] {
        m_outputSettings = OutputSettings::load();
        const int secs = m_outputSettings.replayBufferSeconds;
        m_preview->setReplayBufferSeconds(secs);
        m_audio->setReplayBufferSeconds(secs);
        flash(tr("Recording settings applied"), 2500);
    });
    // Settings ▸ Audio drives the live master-bus limiter.
    connect(m_settings, &SettingsWorkspace::audioLimiterChanged, this,
            [this](bool enabled, double thresholdDb) {
        if (m_audio) {
            m_audio->setLimiterEnabled(enabled);
            m_audio->setLimiterThresholdDb(static_cast<float>(thresholdDb));
        }
    });
    // Apply the persisted limiter state on launch so it survives restarts.
    if (m_audio) {
        QSettings limiterSettings;
        m_audio->setLimiterEnabled(
            limiterSettings.value(QStringLiteral("audio/limiterEnabled"), false).toBool());
        m_audio->setLimiterThresholdDb(static_cast<float>(
            limiterSettings.value(QStringLiteral("audio/limiterThresholdDb"), -3.0).toDouble()));
    }
    // Settings ▸ Hotkeys rebinds global shortcuts on the live HotkeyManager.
    connect(m_settings, &SettingsWorkspace::hotkeyChanged, this,
            [this](const QString& actionId, const QKeySequence& seq) {
        if (m_hotkeys) m_hotkeys->setBinding(actionId, seq);
    });
    m_shell->addWorkspace(QStringLiteral("settings"), m_settings);

    setCentralWidget(m_shell);
    m_shell->setCurrentWorkspace(QStringLiteral("dashboard"));

    connect(m_shell->header(), &WorkspaceHeader::projectPillClicked,
            this, &MainWindow::openProject);
    connect(m_shell, &AppShell::workspaceChanged, this, [this](const QString& id) {
        if (id == QLatin1String("record")) updateStatusBar();
    });

    m_onboarding = new OnboardingOverlay(m_shell);

    // Command palette (Ctrl+K) overlaying the shell.
    m_palette = new CommandPalette(m_shell);
    {
        QVector<CommandPalette::Command> cmds;
        for (const NavItem& n : navItems()) {
            const QString id = n.id;
            cmds.push_back({QStringLiteral("go.") + id, tr("Go to %1").arg(n.label),
                            tr("Workspace"), n.icon,
                            [this, id] { m_shell->setCurrentWorkspace(id); }});
        }
        cmds.push_back({QStringLiteral("rec.toggle"), tr("Start / Stop Recording"),
                        tr("Capture · F9"), QStringLiteral("record"),
                        [this] { m_controlsBar->toggleRecord(); }});
        cmds.push_back({QStringLiteral("stream.toggle"), tr("Go Live / End Stream"),
                        tr("Capture · F8"), QStringLiteral("stream"),
                        [this] { m_controlsBar->toggleStream(); }});
        cmds.push_back({QStringLiteral("proj.new"), tr("New Project"),
                        tr("Project · Ctrl+N"), QStringLiteral("plus"),
                        [this] { newProject(); }});
        cmds.push_back({QStringLiteral("proj.open"), tr("Open Project…"),
                        tr("Project · Ctrl+O"), QStringLiteral("folder"),
                        [this] { openProject(); }});
        cmds.push_back({QStringLiteral("proj.save"), tr("Save Project"),
                        tr("Project · Ctrl+S"), QStringLiteral("download"),
                        [this] { saveProject(); }});
        cmds.push_back({QStringLiteral("setup"), tr("Run Setup Wizard"),
                        tr("Help"), QStringLiteral("sparkle"),
                        [this] { m_onboarding->openWizard(); }});
        m_palette->setCommands(cmds);
    }
    connect(m_shell->header(), &WorkspaceHeader::commandPaletteRequested,
            this, [this] { m_palette->openPalette(); });
    auto* paletteSc = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_K), this);
    connect(paletteSc, &QShortcut::activated, this, [this] { m_palette->openPalette(); });

    // First-run setup wizard (shows once; relaunch from Help or the palette).
    QSettings onboardSettings;
    if (!onboardSettings.value(QStringLiteral("onboarding/completed"), false).toBool()) {
        onboardSettings.setValue(QStringLiteral("onboarding/completed"), true);
        QTimer::singleShot(0, this, [this] { m_onboarding->openWizard(); });
    }
}

void MainWindow::setupMenus() {
    auto* fileMenu = menuBar()->addMenu(tr("&File"));

    auto* newAction = fileMenu->addAction(tr("&New Project"));
    newAction->setShortcut(QKeySequence::New);
    connect(newAction, &QAction::triggered, this, &MainWindow::newProject);

    auto* openAction = fileMenu->addAction(tr("&Open Project..."));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::openProject);

    m_saveAction = fileMenu->addAction(tr("&Save"));
    m_saveAction->setShortcut(QKeySequence::Save);
    connect(m_saveAction, &QAction::triggered, this, &MainWindow::saveProject);

    auto* saveAsAction = fileMenu->addAction(tr("Save &As..."));
    saveAsAction->setShortcut(QKeySequence::SaveAs);
    connect(saveAsAction, &QAction::triggered, this, &MainWindow::saveProjectAs);

    fileMenu->addSeparator();

    auto* exitAction = fileMenu->addAction(tr("E&xit"));
    exitAction->setShortcut(QKeySequence::Quit);
    connect(exitAction, &QAction::triggered, this, &QWidget::close);

    auto* editMenu = menuBar()->addMenu(tr("&Edit"));
    auto* undoAction = m_undoStack->createUndoAction(this, tr("&Undo"));
    undoAction->setShortcut(QKeySequence::Undo);
    editMenu->addAction(undoAction);
    auto* redoAction = m_undoStack->createRedoAction(this, tr("&Redo"));
    redoAction->setShortcut(QKeySequence::Redo);
    editMenu->addAction(redoAction);

    editMenu->addSeparator();

    auto* outputSettingsAction = editMenu->addAction(tr("Output Settings…"));
    connect(outputSettingsAction, &QAction::triggered, this, [this] {
        OutputSettingsDialog dlg(m_outputSettings, this);
        if (dlg.exec() == QDialog::Accepted) {
            m_outputSettings = dlg.settings();
            m_outputSettings.save();
            // Propagate replay buffer duration to the live sources.
            const int secs = m_outputSettings.replayBufferSeconds;
            m_preview->setReplayBufferSeconds(secs);
            m_audio->setReplayBufferSeconds(secs);
        }
    });

    auto* streamSettingsAction = editMenu->addAction(tr("Stream Settings…"));
    connect(streamSettingsAction, &QAction::triggered, this, [this] {
        StreamSettingsDialog dlg(m_streamSettings, this);
        if (dlg.exec() == QDialog::Accepted) {
            m_streamSettings = dlg.settings();
            // key is saved by the dialog itself (CredWriteW); no extra save needed
        }
    });

    auto* hotkeysAction = editMenu->addAction(tr("Hotkeys…"));
    connect(hotkeysAction, &QAction::triggered, this, [this] {
        // v7 Tier 4: pass AudioController so the dialog can enumerate audio
        // inputs and offer per-source Mute bindings alongside the global hotkeys.
        HotkeysDialog dlg(m_hotkeys, m_audio, this);
        dlg.exec();
    });

    auto* viewMenu = menuBar()->addMenu(tr("&View"));

    // Workspace switching from the menu (mirrors the icon rail).
    auto* gotoMenu = viewMenu->addMenu(tr("&Go to"));
    gotoMenu->addAction(tr("Dashboard"), this, [this] { m_shell->setCurrentWorkspace(QStringLiteral("dashboard")); });
    gotoMenu->addAction(tr("Recording"), this, [this] { m_shell->setCurrentWorkspace(QStringLiteral("record")); });
    gotoMenu->addAction(tr("Streaming"), this, [this] { m_shell->setCurrentWorkspace(QStringLiteral("stream")); });

    viewMenu->addSeparator();

    m_studioModeAction = viewMenu->addAction(tr("&Studio Mode"));
    m_studioModeAction->setCheckable(true);
    m_studioModeAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_T));
    m_studioModeAction->setStatusTip(tr("Toggle studio mode (program / staged preview split)"));
    // Persist and restore studio mode preference
    {
        QSettings settings;
        m_studioModeAction->setChecked(settings.value(QStringLiteral("ui/studioMode"), false).toBool());
        if (m_studioModeAction->isChecked())
            m_scenes->setStudioMode(true);
    }
    connect(m_studioModeAction, &QAction::toggled, this, [this](bool on) {
        m_scenes->setStudioMode(on);
        QSettings().setValue(QStringLiteral("ui/studioMode"), on);
    });

    auto* helpMenu = menuBar()->addMenu(tr("&Help"));
    auto* setupAction = helpMenu->addAction(tr("Setup &Wizard…"));
    connect(setupAction, &QAction::triggered, this, [this] { m_onboarding->openWizard(); });
    helpMenu->addSeparator();
    auto* aboutAction = helpMenu->addAction(tr("&About MalloyStudio"));
    connect(aboutAction, &QAction::triggered, this, [this] {
        QMessageBox::about(this, tr("About MalloyStudio"),
            tr("<h3>MalloyStudio</h3>"
               "<p>A C++ OBS-style streaming studio.</p>"
               "<p>Built with Qt 6 + DXGI Desktop Duplication.</p>"));
    });
}

void MainWindow::connectModelSignals() {
    // Basic scene-list changes — just refresh UI text
    auto refresh = [this] {
        updateStatusBar();
        updatePreviewLabel();
    };
    connect(m_scenes, &SceneCollection::sceneAdded,   this, refresh);
    connect(m_scenes, &SceneCollection::sceneRemoved, this, refresh);
    connect(m_scenes, &SceneCollection::sceneRenamed, this, refresh);
    connect(m_scenes, &SceneCollection::itemsChanged, this, refresh);
    connect(m_scenes, &SceneCollection::collectionReset, this, refresh);
    connect(m_undoStack, &QUndoStack::cleanChanged, this, [this](bool){ updateWindowTitle(); });

    // Transition button: promote staged → program with optional fade
    connect(m_transitionBtn, &QPushButton::clicked, this, [this] {
        if (m_controlsBar->transitionType() == ControlsBar::TransitionType::Fade)
            m_pendingTransFrom = m_preview->renderCurrentScene();
        m_scenes->promotePreviewToProgram();
        if (!m_pendingTransFrom.isNull()) {
            m_preview->beginFadeTransition(
                std::move(m_pendingTransFrom),
                m_controlsBar->transitionDurationMs());
            m_pendingTransFrom = {};
        }
    });

    // Update transition button label to match the ControlsBar transition type
    connect(m_controlsBar, &ControlsBar::transitionTypeChanged, this,
            [this](ControlsBar::TransitionType type) {
        m_transitionBtn->setText(
            type == ControlsBar::TransitionType::Fade ? tr("▶ Fade") : tr("▶ Cut"));
    });

    // Step 1 of transition (single-canvas mode): capture "from" frame before scene switches
    connect(m_scenes, &SceneCollection::sceneAboutToChange,
            this, [this](int /*from*/, int /*to*/) {
        // Only capture in live (non-studio) mode; studio mode uses the transition button.
        if (!m_scenes->studioMode() && m_controlsBar->transitionType() == ControlsBar::TransitionType::Fade)
            m_pendingTransFrom = m_preview->renderCurrentScene();
    });

    // Step 2: after the model switches, kick off the crossfade
    connect(m_scenes, &SceneCollection::currentChanged, this, [this](int) {
        if (!m_pendingTransFrom.isNull()) {
            m_preview->beginFadeTransition(
                std::move(m_pendingTransFrom),
                m_controlsBar->transitionDurationMs());
            m_pendingTransFrom = {};
        }
        updateStatusBar();
        updatePreviewLabel();
    });

    // Capture controller → both preview panes (display + window)
    connect(m_captureController, &CaptureController::frameReady,
            m_preview, &PreviewWidget::updateFrame);
    connect(m_captureController, &CaptureController::frameCleared,
            m_preview, &PreviewWidget::clearFrame);
    connect(m_captureController, &CaptureController::windowFrameReady,
            m_preview, &PreviewWidget::updateWindowFrame);
    connect(m_captureController, &CaptureController::windowFrameCleared,
            m_preview, &PreviewWidget::clearWindowFrame);
    connect(m_captureController, &CaptureController::cameraFrameReady,
            m_preview, &PreviewWidget::updateCameraFrame);
    connect(m_captureController, &CaptureController::cameraFrameCleared,
            m_preview, &PreviewWidget::clearCameraFrame);
    // Staged preview also needs live frames so the user can see what they're staging.
    connect(m_captureController, &CaptureController::frameReady,
            m_stagePreview, &PreviewWidget::updateFrame);
    connect(m_captureController, &CaptureController::frameCleared,
            m_stagePreview, &PreviewWidget::clearFrame);
    connect(m_captureController, &CaptureController::windowFrameReady,
            m_stagePreview, &PreviewWidget::updateWindowFrame);
    connect(m_captureController, &CaptureController::windowFrameCleared,
            m_stagePreview, &PreviewWidget::clearWindowFrame);
    connect(m_captureController, &CaptureController::cameraFrameReady,
            m_stagePreview, &PreviewWidget::updateCameraFrame);
    connect(m_captureController, &CaptureController::cameraFrameCleared,
            m_stagePreview, &PreviewWidget::clearCameraFrame);
    connect(m_captureController, &CaptureController::statusChanged,
            this, [this](const QString& summary) {
        m_captureStatus = summary;
        updateStatusBar();
    });

    // SceneCollection audio changes → AudioController dynamic mic management
    connect(m_scenes, &SceneCollection::audioInputsChanged, this, [this] {
        m_audio->reconcileInputs(m_scenes->gatherVisibleAudioIds());
    });

    // ControlsBar → MediaController (recording)
    connect(m_controlsBar, &ControlsBar::recordingStarted, this, [this] {
        if (!m_media->ffmpegAvailable()) {
            QMessageBox::warning(this, tr("Recording Unavailable"),
                tr("ffmpeg.exe was not found in PATH. Install ffmpeg to enable recording."));
            m_controlsBar->forceStopRecording();
            return;
        }

        QSettings settings;
        const QString lastDir = settings.value(
            QStringLiteral("recording/lastDir"),
            QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).toString();
        // Expand the filename pattern from Settings ▸ Recording ▸ Storage.
        // Tokens: {project} {scene} {date} {time}; illegal filename chars stripped.
        const QDateTime nowTs = QDateTime::currentDateTime();
        QString base = settings.value(QStringLiteral("recording/filenamePattern"),
                                      QStringLiteral("{project} — {date} {time}")).toString();
        base.replace(QStringLiteral("{project}"),
                     m_projectPath.isEmpty() ? tr("Untitled")
                                             : QFileInfo(m_projectPath).completeBaseName());
        base.replace(QStringLiteral("{scene}"),
                     (m_scenes && m_scenes->currentScene()) ? m_scenes->currentScene()->name()
                                                            : tr("Scene"));
        base.replace(QStringLiteral("{date}"), nowTs.toString(QStringLiteral("yyyyMMdd")));
        base.replace(QStringLiteral("{time}"), nowTs.toString(QStringLiteral("HHmmss")));
        for (QChar bad : {QChar('<'), QChar('>'), QChar(':'), QChar('"'),
                          QChar('/'), QChar('\\'), QChar('|'), QChar('?'), QChar('*')})
            base.remove(bad);
        base = base.trimmed();
        if (base.isEmpty())
            base = QStringLiteral("MalloyStudio-%1").arg(nowTs.toString(QStringLiteral("yyyyMMdd-HHmmss")));
        const QString ext = m_outputSettings.container.isEmpty()
            ? QStringLiteral("mp4") : m_outputSettings.container;
        const QString suggested = QDir(lastDir).filePath(base + QChar('.') + ext);

        const QString path = QFileDialog::getSaveFileName(
            this, tr("Save Recording"), suggested,
            tr("MP4 Video (*.mp4);;Matroska (*.mkv);;All Files (*)"));
        if (path.isEmpty()) {
            m_controlsBar->forceStopRecording();
            return;
        }
        settings.setValue(QStringLiteral("recording/lastDir"), QFileInfo(path).absolutePath());

        QString err;
        if (!m_media->startRecording(path, m_outputSettings, &err)) {
            QMessageBox::warning(this, tr("Recording Failed"), err);
            m_controlsBar->forceStopRecording();
            return;
        }
        flash(tr("Recording to %1").arg(QFileInfo(path).fileName()));
        updateShellMode();
    });

    connect(m_controlsBar, &ControlsBar::recordingStopped, this, [this] {
        m_media->stopRecording();
        updateShellMode();
    });

    connect(m_media, &MediaController::recordingFinished,
            this, [this](const QString& path, qint64 bytes) {
        m_controlsBar->forceStopRecording();
        updateShellMode();
        m_mediaRegistry->addSearchDir(QFileInfo(path).absolutePath());
        flash(tr("Saved %1 (%2 KB)").arg(QFileInfo(path).fileName()).arg(bytes / 1024), 5000);
    });

    // ControlsBar → MediaController (streaming)
    connect(m_controlsBar, &ControlsBar::streamingStarted, this, [this] {
        if (!m_media->ffmpegAvailable()) {
            QMessageBox::warning(this, tr("Streaming Unavailable"),
                tr("ffmpeg.exe was not found in PATH. Install ffmpeg to enable streaming."));
            m_controlsBar->forceStopStreaming();
            return;
        }

        // Prompt to configure if key is missing
        if (m_streamSettings.streamKey.isEmpty()) {
            StreamSettingsDialog dlg(m_streamSettings, this);
            if (dlg.exec() != QDialog::Accepted) {
                m_controlsBar->forceStopStreaming();
                return;
            }
            m_streamSettings = dlg.settings();
        }

        QString err;
        if (!m_media->startStreaming(m_streamSettings, m_outputSettings, &err)) {
            QMessageBox::warning(this, tr("Streaming Failed"), err);
            m_controlsBar->forceStopStreaming();
            return;
        }
        flash(tr("Streaming to %1").arg(
            StreamSettings::displayName(m_streamSettings.service)));
        updateShellMode();
    });

    connect(m_controlsBar, &ControlsBar::streamingStopped, this, [this] {
        m_media->stopStreaming();
        updateShellMode();
    });

    connect(m_media, &MediaController::streamingFinished, this, [this] {
        m_controlsBar->forceStopStreaming();
        updateShellMode();
        flash(tr("Stream ended."), 4000);
    });

    // v7 Tier 3: live bitrate + dropped-frames updates from ffmpeg → mixer.
    connect(m_media, &MediaController::streamingProgress,
            m_controlsBar, &ControlsBar::setStreamStats);

    connect(m_media, &MediaController::errorOccurred,
            this, [this](const QString& origin, const QString& msg) {
        const QString title = (origin == QStringLiteral("recording"))
            ? tr("Recording Error") : tr("Streaming Error");
        // EncoderPipeline appends "\n\nLast stderr:\n<tail>" when ffmpeg
        // produced stderr output. Split that off into setDetailedText so the
        // wall of ffmpeg output hides behind a "Show Details" button — the
        // surface message stays short and readable.
        QString summary = msg;
        QString detail;
        const int sep = msg.indexOf(QStringLiteral("\n\nLast stderr:\n"));
        if (sep >= 0) {
            summary = msg.left(sep);
            detail  = msg.mid(sep + 2);   // include "Last stderr:" header in details
        }
        QMessageBox box(this);
        box.setIcon(QMessageBox::Critical);
        box.setWindowTitle(title);
        box.setText(summary);
        if (!detail.isEmpty()) box.setDetailedText(detail);
        box.setStandardButtons(QMessageBox::Ok);
        box.exec();
        if (origin == QStringLiteral("recording"))
            m_controlsBar->forceStopRecording();
        else
            m_controlsBar->forceStopStreaming();
    });

    // ── Hotkey dispatch ────────────────────────────────────────────────────
    connect(m_hotkeys, &HotkeyManager::triggered, this,
            [this](const QString& actionId) {
        if (actionId == QLatin1String(HotkeyManager::kRecordToggle)) {
            if (m_media->isRecording())
                m_controlsBar->forceStopRecording();
            // trigger the start flow via the button's click logic
        } else if (actionId == QLatin1String(HotkeyManager::kStreamToggle)) {
            if (m_media->isStreaming())
                m_controlsBar->forceStopStreaming();
        } else if (actionId == QLatin1String(HotkeyManager::kStudioTransition)) {
            m_transitionBtn->click();
        } else if (actionId == QLatin1String(HotkeyManager::kReplaySave)) {
            if (m_outputSettings.replayBufferSeconds <= 0) {
                flash(tr("Replay buffer is disabled — enable it in Output Settings."), 4000);
            } else {
                const QString dir = QStandardPaths::writableLocation(
                    QStandardPaths::MoviesLocation);
                const QString path = QDir(dir).filePath(
                    QStringLiteral("replay-%1.mp4").arg(
                        QDateTime::currentDateTime().toString(
                            QStringLiteral("yyyyMMdd-HHmmss"))));
                // Snapshot both rings before calling saveReplay so the
                // timestamps are as synchronised as possible.
                auto frames = m_preview->snapshotReplayFrames();
                auto pcm    = m_audio->snapshotReplayPcm();
                QString err;
                if (!m_media->saveReplay(path, m_outputSettings,
                                         std::move(frames),
                                         m_preview->nativeWidth(),
                                         m_preview->nativeHeight(),
                                         std::move(pcm), &err))
                    flash(tr("Replay save failed: %1").arg(err), 5000);
                else
                    flash(tr("Saving replay…"), 2000);
            }
        } else if (actionId.startsWith(QStringLiteral("scene.switch."))) {
            const int idx = actionId.mid(13).toInt() - 1;  // 0-based
            if (idx >= 0 && idx < m_scenes->sceneCount())
                m_scenes->setCurrentIndex(idx);
        } else if (actionId.startsWith(QStringLiteral("audio.mute."))) {
            // v7 Tier 4: per-source mute. The action ID format is
            // "audio.mute.<inputId>" where inputId matches AudioInput::id
            // ("loopback:default" or "input:<guid>"). We look up the input and
            // flip its mute flag — same code path the mixer's M button uses.
            const QString audioId = actionId.mid(11);   // strip "audio.mute."
            const auto& inputs = m_audio->inputs();
            auto it = std::find_if(inputs.cbegin(), inputs.cend(),
                [&](const AudioInput& in){ return in.id == audioId; });
            if (it != inputs.cend()) m_audio->setMuted(audioId, !it->muted);
        }
    });

    // Replay saved notification
    connect(m_media, &MediaController::replaySaved, this,
            [this](const QString& path) {
        const QString project = m_projectPath.isEmpty()
            ? QString() : QFileInfo(m_projectPath).completeBaseName();
        m_clipsRegistry->registerFile(path, project, m_outputSettings.replayBufferSeconds);
        flash(tr("Replay saved: %1").arg(QFileInfo(path).fileName()), 5000);
    });
}

bool MainWindow::maybeSave() {
    if (m_undoStack->isClean()) return true;

    QMessageBox box(this);
    box.setWindowTitle(tr("Unsaved Changes"));
    box.setText(tr("Save changes to this MalloyStudio project?"));
    auto* save = box.addButton(tr("Save"), QMessageBox::AcceptRole);
    box.addButton(tr("Discard"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(save);
    box.exec();

    if (box.clickedButton() == static_cast<QAbstractButton*>(save)) return saveProject();
    return box.standardButton(box.clickedButton()) != QMessageBox::Cancel;
}

void MainWindow::newProject() {
    if (!maybeSave()) return;
    m_captureController->stopAll();
    m_scenes->clear();
    if (m_editor) m_editor->setTimelineJson({});
    m_projectPath.clear();
    m_undoStack->clear();
    m_undoStack->setClean();
    updateWindowTitle();
    updateStatusBar();
}

void MainWindow::openProject() {
    if (!maybeSave()) return;

    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open MalloyStudio Project"), QString(),
        tr("MalloyStudio Project (*.malloy.json *.json);;All Files (*)"));
    if (path.isEmpty()) return;
    loadProject(path);
}

bool MainWindow::saveProject() {
    if (m_projectPath.isEmpty()) return saveProjectAs();

    QString error;
    if (!ProjectDocument::saveToFile(*m_scenes,
                                     m_editor ? m_editor->timelineJson() : QJsonArray{},
                                     m_projectPath, &error)) {
        QMessageBox::warning(this, tr("Save Failed"), error);
        return false;
    }

    m_undoStack->setClean();
    updateWindowTitle();
    if (m_projectRegistry) m_projectRegistry->addSearchDir(QFileInfo(m_projectPath).absolutePath());
    flash(tr("Saved %1").arg(QFileInfo(m_projectPath).fileName()), 2500);
    return true;
}

bool MainWindow::saveProjectAs() {
    QString path = QFileDialog::getSaveFileName(
        this, tr("Save MalloyStudio Project"), m_projectPath,
        tr("MalloyStudio Project (*.malloy.json);;JSON Project (*.json);;All Files (*)"));
    if (path.isEmpty()) return false;
    if (!path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive))
        path += QStringLiteral(".malloy.json");
    m_projectPath = path;
    return saveProject();
}

bool MainWindow::loadProject(const QString& filePath) {
    m_captureController->stopAll();

    QString error;
    QJsonArray timeline;
    if (!ProjectDocument::loadFromFile(*m_scenes, &timeline, filePath, &error)) {
        QMessageBox::warning(this, tr("Open Failed"), error);
        return false;
    }
    if (m_editor) m_editor->setTimelineJson(timeline);

    m_projectPath = filePath;
    m_undoStack->clear();
    m_undoStack->setClean();
    updateWindowTitle();
    updateStatusBar();
    if (m_projectRegistry) m_projectRegistry->addSearchDir(QFileInfo(filePath).absolutePath());
    flash(tr("Opened %1").arg(QFileInfo(filePath).fileName()), 2500);
    return true;
}

void MainWindow::updateWindowTitle() {
    const QString name = m_projectPath.isEmpty()
        ? tr("Untitled")
        : QFileInfo(m_projectPath).completeBaseName();
    setWindowTitle(QStringLiteral("%1%2 - MalloyStudio")
        .arg(m_undoStack && !m_undoStack->isClean() ? QStringLiteral("*") : QString())
        .arg(name));
    if (m_shell) m_shell->header()->setProjectName(name);
    if (m_dashboard) m_dashboard->setProjectName(name);
}

void MainWindow::updateStatusBar() {
    // The capture summary used to live in the OS status bar; surface the live
    // scene/source counts in the workspace header subtitle while on Recording.
    if (!m_shell) return;
    const QString summary = tr("%1 scenes · %2 sources")
        .arg(m_scenes->sceneCount())
        .arg(m_scenes->sourceCount());
    if (m_shell->currentWorkspace() == QLatin1String("record"))
        m_shell->header()->setSubtitle(summary);
}

void MainWindow::updateShellMode() {
    if (!m_shell) return;
    const bool streaming = m_media && m_media->isStreaming();
    StudioStatusBar::Mode mode = StudioStatusBar::Mode::Idle;
    if (streaming)                                       mode = StudioStatusBar::Mode::Streaming;
    else if (m_media && m_media->isRecording())          mode = StudioStatusBar::Mode::Recording;
    else if (m_renderQueue && m_renderQueue->hasActive()) mode = StudioStatusBar::Mode::Rendering;
    m_shell->status()->setMode(mode);
    if (m_streamStudio) m_streamStudio->setLive(streaming);
    if (m_dashboard) {
        m_dashboard->setRecording(m_media && m_media->isRecording());
        m_dashboard->setStreaming(streaming);
    }
}

void MainWindow::flash(const QString& text, int ms) {
    if (m_shell) m_shell->status()->flashMessage(text, ms);
}

void MainWindow::updatePreviewLabel() {
    Scene* cur = m_scenes->currentScene();
    m_preview->setSceneName(cur ? cur->name() : QString());
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (!maybeSave()) {
        event->ignore();
        return;
    }
    m_captureController->stopAll();
    QSettings settings;
    settings.setValue("geometry", saveGeometry());
    settings.setValue("windowState", saveState());
    QMainWindow::closeEvent(event);
}
