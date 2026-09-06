#pragma once

#include <QHash>
#include <QString>
#include <QWidget>

class AudioController;
class ClipsRegistry;
class MeterBar;
class Placeholder;
class ProjectRegistry;
class RenderQueue;
class SceneCollection;
class QGridLayout;
class QLabel;
class QPushButton;
class QVBoxLayout;

// Project-hub home screen (dashboard.jsx): hero, quick actions, preview
// signal, recent recordings/projects/clips, render queue and system status.
//
// Every panel reads live state: the scene collection, the audio mix bus, the
// clip and project registries, the render queue, the recording output folder
// and the persisted output/stream settings. Panels that have nothing to show
// render an empty state rather than invented rows, so the screen never claims
// content the install does not have.
//
// Emits navigateTo() / recordRequested() / streamRequested() /
// saveReplayRequested() / newProjectRequested() / openProjectRequested() so
// MainWindow drives the real workspaces and pipelines.
class Dashboard : public QWidget {
    Q_OBJECT
public:
    // scenes/audio may be null only in isolated tests; the panels degrade to
    // empty states. MainWindow always passes the live instances.
    Dashboard(SceneCollection* scenes,
              AudioController* audio,
              ClipsRegistry* clips,
              ProjectRegistry* projects,
              RenderQueue* renderQueue,
              QWidget* parent = nullptr);

    void setProjectName(const QString& name);
    void setRecording(bool on);
    void setStreaming(bool on);

signals:
    void navigateTo(const QString& workspaceId);
    void recordRequested();
    void streamRequested();
    void saveReplayRequested();
    void newProjectRequested();
    void openProjectRequested(const QString& filePath);

protected:
    // Panels re-read their sources on show: settings and the output folder can
    // change from other workspaces while the dashboard is stacked behind them.
    void showEvent(QShowEvent* event) override;

private slots:
    void rebuildMeterRows();                                   // mixer inputs changed
    void onLevels(const QString& id, float peakL, float peakR);
    void refreshRecordings();
    void refreshProjects();
    void refreshClips();
    void refreshRenderQueue();
    void refreshSystemStatus();
    void refreshSceneSummary();

private:
    QWidget* buildHero();
    QWidget* buildQuickActions();
    QWidget* buildPreviewSignal();
    QWidget* buildRecentRecordings();
    QWidget* buildRenderQueue();
    QWidget* buildRecentProjects();
    QWidget* buildRecentClips();
    QWidget* buildSystemStatus();

    void refreshAll();
    void refreshOutputFacts();     // hero facts + preview header, from settings
    void refreshState();           // record/live button captions

    // One live meter per mix-bus input, addressed by AudioInput::id.
    struct MeterRow {
        MeterBar* meter = nullptr;
        QLabel*   db    = nullptr;
        double    level = 0.0;
        double    peak  = 0.0;
    };

    SceneCollection* m_scenes       = nullptr;
    AudioController* m_audio        = nullptr;
    ClipsRegistry*   m_clips        = nullptr;
    ProjectRegistry* m_projects     = nullptr;
    RenderQueue*     m_renderQueue  = nullptr;

    QLabel*      m_heroTitle = nullptr;
    QLabel*      m_heroSubtitle = nullptr;
    QPushButton* m_recBtn = nullptr;
    QPushButton* m_liveBtn = nullptr;
    // "Clip last N s" quick action: both captions track the configured
    // replay-buffer length so the button never advertises a length the
    // buffer is not holding.
    QLabel*      m_clipTitle = nullptr;
    QLabel*      m_clipSub = nullptr;
    bool m_recording = false;
    bool m_streaming = false;

    QLabel*      m_factResolution = nullptr;
    QLabel*      m_factFramerate  = nullptr;
    QLabel*      m_factAudio      = nullptr;
    QLabel*      m_factMic        = nullptr;
    QLabel*      m_factDestination = nullptr;

    QLabel*       m_previewMeta = nullptr;
    Placeholder*  m_scenePlaceholder = nullptr;
    QVBoxLayout*  m_meterLayout = nullptr;
    QHash<QString, MeterRow> m_meterRows;

    QVBoxLayout*  m_recordingsLayout = nullptr;
    QLabel*       m_renderMeta = nullptr;         // header "N active · M pending"
    QVBoxLayout*  m_renderBodyLayout = nullptr;
    QGridLayout*  m_projectsLayout = nullptr;
    QGridLayout*  m_clipsLayout = nullptr;
    QGridLayout*  m_statusLayout = nullptr;
    QLabel*       m_statusMeta = nullptr;
};
