#pragma once

// RenderQueue — background render job model (audit.html's RenderQueue). Holds
// jobs with state + progress, persists them as JSON, and drives progress with a
// simulated worker (one active job at a time) so the Render workspace is fully
// functional ahead of the real EncoderPipeline integration. Completed/failed
// jobs persist as history; active/pending resume as pending on next launch.

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QVector>

class QJsonObject;
class QTimer;

struct RenderJob {
    enum State { Pending, Active, Completed, Failed };

    QString   id;
    QString   name;
    QString   project;
    QString   target;        // e.g. "1080p60 · NVENC · 24 Mb/s"
    QString   outputPath;
    QString   error;         // set when Failed
    State     state = Pending;
    int       progress = 0;  // 0..100
    QDateTime finishedAt;

    QString stateText() const;
    QJsonObject toJson() const;
    static RenderJob fromJson(const QJsonObject& o);
};

class RenderQueue : public QObject {
    Q_OBJECT
public:
    explicit RenderQueue(QObject* parent = nullptr);

    void setStorePath(const QString& path);   // tests: settings-free location

    const QVector<RenderJob>& jobs() const { return m_jobs; }
    int  countOfState(RenderJob::State s) const;
    bool hasActive() const;

    QString enqueue(const QString& name, const QString& target,
                    const QString& project, const QString& outputPath = QString());
    void retry(const QString& id);   // Failed → Pending
    void cancel(const QString& id);  // Active/Pending → removed
    void clearCompleted();
    void setPaused(bool paused);
    bool paused() const { return m_paused; }

    // Testing hook: advance the active job by one simulated tick.
    void advanceForTest() { tick(); }

signals:
    void changed();

private:
    void tick();
    void startNext();
    void load();
    void save() const;
    QString resolvedStorePath() const;

    QVector<RenderJob> m_jobs;
    QTimer* m_timer = nullptr;
    bool    m_paused = false;
    QString m_storePath;
};
