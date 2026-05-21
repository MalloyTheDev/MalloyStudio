#pragma once

#include <QString>
#include <QWidget>

class MeterBar;
class QLabel;
class QTimer;

// Project-hub home screen (dashboard.jsx): hero, quick actions, now-playing
// meters, recent recordings/projects/clips, render queue, and system status.
// Emits navigateTo() so MainWindow can switch workspaces from the hero/quick
// actions. Demo content is static where no live data source exists yet.
class Dashboard : public QWidget {
    Q_OBJECT
public:
    explicit Dashboard(QWidget* parent = nullptr);

    void setProjectName(const QString& name);

signals:
    void navigateTo(const QString& workspaceId);

private:
    QWidget* buildHero();
    QWidget* buildQuickActions();
    QWidget* buildNowPlaying();
    QWidget* buildRecentRecordings();
    QWidget* buildRenderQueue();
    QWidget* buildRecentProjects();
    QWidget* buildRecentClips();
    QWidget* buildSystemStatus();
    void tickMeters();

    QLabel*   m_heroTitle = nullptr;
    MeterBar* m_micMeter = nullptr;
    MeterBar* m_deskMeter = nullptr;
    QLabel*   m_micDb = nullptr;
    QLabel*   m_deskDb = nullptr;
    QTimer*   m_meterTimer = nullptr;

    double m_micL = 0.4, m_micP = 0.4, m_deskL = 0.55, m_deskP = 0.55;
};
