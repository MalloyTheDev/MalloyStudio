#pragma once

// RenderQueue - background render job model. Holds jobs with state + progress,
// persists them as JSON, and runs one at a time. Completed and failed jobs
// persist as history; anything active at shutdown resumes as pending, because a
// killed render leaves nothing to resume from.
//
// A job carries everything needed to render it: structured encoder settings and
// a snapshot of the timeline taken when it was queued
// (docs/adr/0002-render-job-contract.md). Editing the project afterwards does
// not change what a queued job produces, and a retry re-renders the same thing
// that failed.
//
// Jobs are rendered by RenderPipeline, one ffmpeg process at a time
// (docs/adr/0003-render-worker-pipeline.md). The queue owns the scheduling and
// the persistence; the pipeline owns the process.

#include "recording/OutputSettings.h"

#include <QDateTime>
#include <QJsonArray>
#include <QObject>
#include <QString>
#include <QVector>

class QJsonObject;
class RenderPipeline;

struct RenderJob {
    enum State { Pending, Active, Completed, Failed };

    QString   id;
    QString   name;
    QString   project;       // display name of the source project
    QString   projectPath;   // .malloy.json it came from; informational
    QString   target;        // display string derived from `output` at enqueue
    QString   outputPath;    // full path of the file to write, not a directory
    QString   error;         // set when Failed
    State     state = Pending;
    int       progress = 0;  // 0..100
    QDateTime finishedAt;

    OutputSettings output;   // encoder settings this job renders with
    QJsonArray     timeline; // snapshot of the clips to render

    // False for jobs written before the schema existed, which cannot be
    // rendered because nothing recorded what they were rendering.
    bool renderable() const { return !timeline.isEmpty() && !outputPath.isEmpty(); }

    QString stateText() const;
    QJsonObject toJson() const;
    static RenderJob fromJson(const QJsonObject& o);
};

// Everything needed to queue a render. Grouped so the call site reads as a
// request rather than six positional strings.
struct RenderRequest {
    QString        name;
    QString        project;
    QString        projectPath;
    QString        outputPath;
    OutputSettings output;
    QJsonArray     timeline;
};

class RenderQueue : public QObject {
    Q_OBJECT
public:
    explicit RenderQueue(QObject* parent = nullptr);

    void setStorePath(const QString& path);   // tests: settings-free location

    const QVector<RenderJob>& jobs() const { return m_jobs; }
    int  countOfState(RenderJob::State s) const;
    bool hasActive() const;

    // Returns the new job id, or an empty string with *error set when the
    // request cannot produce a file: no timeline, no output path, or an output
    // directory that does not exist. Failing here beats failing minutes later
    // inside the encoder.
    QString enqueue(const RenderRequest& request, QString* error = nullptr);

    // Display string for a job's encoder settings, e.g.
    // "1920x1080 60fps libx264 CRF 23". Public so the enqueue call sites and
    // tests agree on one rendering of the same settings.
    static QString describeTarget(const OutputSettings& output);
    void retry(const QString& id);   // Failed → Pending
    void cancel(const QString& id);  // Active/Pending → removed
    void clearCompleted();
    void setPaused(bool paused);
    bool paused() const { return m_paused; }

signals:
    void changed();

private:
    void startNext();          // promote and start the next pending job
    RenderJob* jobById(const QString& id);
    void load();
    void save() const;
    QString resolvedStorePath() const;

    QVector<RenderJob> m_jobs;
    RenderPipeline* m_pipeline = nullptr;
    QString m_activeId;        // job the pipeline is currently rendering
    bool    m_paused = false;
    QString m_storePath;
};
