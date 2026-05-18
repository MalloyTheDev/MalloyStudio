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
#include "audio/AudioController.h"
#include "input/HotkeyManager.h"
#include "recording/MediaController.h"
#include "model/SceneCollection.h"
#include "model/Scene.h"
#include "capture/CaptureController.h"
#include "project/ProjectDocument.h"

#include <QAction>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QToolBar>
#include <QUndoStack>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    resize(1280, 720);

    m_scenes           = new SceneCollection(this);
    m_undoStack        = new QUndoStack(this);
    m_scenes->setUndoStack(m_undoStack);
    m_captureController = new CaptureController(m_scenes, this);
    m_audio             = new AudioController(this);
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

    setCentralWidget(m_studioContainer);

    // Wire transition column visibility to studioMode toggle
    connect(m_scenes, &SceneCollection::studioModeChanged, this,
            [this, transCol](bool on) {
        m_stagePreview->setVisible(on);
        transCol->setVisible(on);
    });

    m_scenesPanel = new ScenesPanel(m_scenes, this);
    m_scenesDock = new QDockWidget(tr("Scenes"), this);
    m_scenesDock->setObjectName("ScenesDock");
    m_scenesDock->setWidget(m_scenesPanel);
    m_scenesDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::LeftDockWidgetArea, m_scenesDock);

    m_sourcesPanel = new SourcesPanel(m_scenes, this);
    m_sourcesDock = new QDockWidget(tr("Sources"), this);
    m_sourcesDock->setObjectName("SourcesDock");
    m_sourcesDock->setWidget(m_sourcesPanel);
    m_sourcesDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::RightDockWidgetArea, m_sourcesDock);

    m_inspectorPanel = new InspectorPanel(m_scenes, m_captureController, m_audio, this);
    m_inspectorDock = new QDockWidget(tr("Inspector"), this);
    m_inspectorDock->setObjectName("InspectorDock");
    m_inspectorDock->setWidget(m_inspectorPanel);
    m_inspectorDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::RightDockWidgetArea, m_inspectorDock);
    splitDockWidget(m_sourcesDock, m_inspectorDock, Qt::Vertical);

    m_mixerPanel = new AudioMixerPanel(m_audio, this);
    m_mixerDock = new QDockWidget(tr("Audio Mixer"), this);
    m_mixerDock->setObjectName("AudioMixerDock");
    m_mixerDock->setWidget(m_mixerPanel);
    m_mixerDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    addDockWidget(Qt::BottomDockWidgetArea, m_mixerDock);

    m_controlsBar = new ControlsBar(this);
    auto* bar = addToolBar(tr("Controls"));
    bar->setObjectName(QStringLiteral("ControlsToolBar"));
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->addWidget(m_controlsBar);
    addToolBar(Qt::BottomToolBarArea, bar);
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
        HotkeysDialog dlg(m_hotkeys, this);
        dlg.exec();
    });

    auto* viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(m_scenesDock->toggleViewAction());
    viewMenu->addAction(m_sourcesDock->toggleViewAction());
    viewMenu->addAction(m_inspectorDock->toggleViewAction());
    viewMenu->addAction(m_mixerDock->toggleViewAction());

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
    // Staged preview also needs live frames so the user can see what they're staging.
    connect(m_captureController, &CaptureController::frameReady,
            m_stagePreview, &PreviewWidget::updateFrame);
    connect(m_captureController, &CaptureController::frameCleared,
            m_stagePreview, &PreviewWidget::clearFrame);
    connect(m_captureController, &CaptureController::windowFrameReady,
            m_stagePreview, &PreviewWidget::updateWindowFrame);
    connect(m_captureController, &CaptureController::windowFrameCleared,
            m_stagePreview, &PreviewWidget::clearWindowFrame);
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
        const QString suggested = QDir(lastDir).filePath(
            QStringLiteral("MalloyStudio-%1.mp4").arg(
                QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));

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
        statusBar()->showMessage(tr("Recording to %1").arg(QFileInfo(path).fileName()));
    });

    connect(m_controlsBar, &ControlsBar::recordingStopped, this, [this] {
        m_media->stopRecording();
    });

    connect(m_media, &MediaController::recordingFinished,
            this, [this](const QString& path, qint64 bytes) {
        m_controlsBar->forceStopRecording();
        statusBar()->showMessage(
            tr("Saved %1 (%2 KB)").arg(QFileInfo(path).fileName()).arg(bytes / 1024), 5000);
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
        statusBar()->showMessage(tr("Streaming to %1").arg(
            StreamSettings::displayName(m_streamSettings.service)));
    });

    connect(m_controlsBar, &ControlsBar::streamingStopped, this, [this] {
        m_media->stopStreaming();
    });

    connect(m_media, &MediaController::streamingFinished, this, [this] {
        m_controlsBar->forceStopStreaming();
        statusBar()->showMessage(tr("Stream ended."), 4000);
    });

    connect(m_media, &MediaController::errorOccurred,
            this, [this](const QString& origin, const QString& msg) {
        const QString title = (origin == QStringLiteral("recording"))
            ? tr("Recording Error") : tr("Streaming Error");
        QMessageBox::warning(this, title, msg);
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
                statusBar()->showMessage(
                    tr("Replay buffer is disabled — enable it in Output Settings."), 4000);
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
                    statusBar()->showMessage(tr("Replay save failed: %1").arg(err), 5000);
                else
                    statusBar()->showMessage(tr("Saving replay…"), 2000);
            }
        } else if (actionId.startsWith(QStringLiteral("scene.switch."))) {
            const int idx = actionId.mid(13).toInt() - 1;  // 0-based
            if (idx >= 0 && idx < m_scenes->sceneCount())
                m_scenes->setCurrentIndex(idx);
        }
    });

    // Replay saved notification
    connect(m_media, &MediaController::replaySaved, this,
            [this](const QString& path) {
        statusBar()->showMessage(
            tr("Replay saved: %1").arg(QFileInfo(path).fileName()), 5000);
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
    if (!ProjectDocument::saveToFile(*m_scenes, m_projectPath, &error)) {
        QMessageBox::warning(this, tr("Save Failed"), error);
        return false;
    }

    m_undoStack->setClean();
    updateWindowTitle();
    statusBar()->showMessage(tr("Saved %1").arg(QFileInfo(m_projectPath).fileName()), 2500);
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
    if (!ProjectDocument::loadFromFile(*m_scenes, filePath, &error)) {
        QMessageBox::warning(this, tr("Open Failed"), error);
        return false;
    }

    m_projectPath = filePath;
    m_undoStack->clear();
    m_undoStack->setClean();
    updateWindowTitle();
    updateStatusBar();
    statusBar()->showMessage(tr("Opened %1").arg(QFileInfo(filePath).fileName()), 2500);
    return true;
}

void MainWindow::updateWindowTitle() {
    const QString name = m_projectPath.isEmpty()
        ? tr("Untitled")
        : QFileInfo(m_projectPath).completeBaseName();
    setWindowTitle(QStringLiteral("%1%2 - MalloyStudio")
        .arg(m_undoStack && !m_undoStack->isClean() ? QStringLiteral("*") : QString())
        .arg(name));
}

void MainWindow::updateStatusBar() {
    statusBar()->showMessage(tr("Scenes: %1   Layers: %2   Sources: %3   %4")
        .arg(m_scenes->sceneCount())
        .arg(m_scenes->totalSourceCount())
        .arg(m_scenes->sourceCount())
        .arg(m_captureStatus));
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
