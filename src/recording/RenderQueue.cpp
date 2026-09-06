#include "recording/RenderQueue.h"
#include "recording/EncoderRegistry.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#include <algorithm>

namespace {
// Bumped when the store layout changes in a way a reader must know about.
// Schema 0 (a bare array) is still read.
constexpr int kStoreSchema = 1;
}  // namespace

QString RenderJob::stateText() const {
    switch (state) {
    case Pending:   return QStringLiteral("Pending");
    case Active:    return QStringLiteral("Active");
    case Completed: return QStringLiteral("Completed");
    case Failed:    return QStringLiteral("Failed");
    }
    return QString();
}

QJsonObject RenderJob::toJson() const {
    QJsonObject o;
    o.insert(QStringLiteral("id"), id);
    o.insert(QStringLiteral("name"), name);
    o.insert(QStringLiteral("project"), project);
    o.insert(QStringLiteral("projectPath"), projectPath);
    o.insert(QStringLiteral("target"), target);
    o.insert(QStringLiteral("outputPath"), outputPath);
    o.insert(QStringLiteral("error"), error);
    o.insert(QStringLiteral("state"), int(state));
    o.insert(QStringLiteral("progress"), progress);
    o.insert(QStringLiteral("finishedAt"), finishedAt.toString(Qt::ISODate));
    o.insert(QStringLiteral("output"), output.toJson());
    o.insert(QStringLiteral("timeline"), timeline);
    return o;
}

RenderJob RenderJob::fromJson(const QJsonObject& o) {
    RenderJob j;
    j.id         = o.value(QStringLiteral("id")).toString();
    j.name       = o.value(QStringLiteral("name")).toString();
    j.project    = o.value(QStringLiteral("project")).toString();
    j.projectPath = o.value(QStringLiteral("projectPath")).toString();
    j.target     = o.value(QStringLiteral("target")).toString();
    j.outputPath = o.value(QStringLiteral("outputPath")).toString();
    j.error      = o.value(QStringLiteral("error")).toString();
    j.state      = static_cast<State>(o.value(QStringLiteral("state")).toInt());
    j.progress   = o.value(QStringLiteral("progress")).toInt();
    j.finishedAt = QDateTime::fromString(o.value(QStringLiteral("finishedAt")).toString(), Qt::ISODate);
    j.output     = OutputSettings::fromJson(o.value(QStringLiteral("output")).toObject());
    j.timeline   = o.value(QStringLiteral("timeline")).toArray();
    return j;
}

RenderQueue::RenderQueue(QObject* parent) : QObject(parent) {
    m_timer = new QTimer(this);
    m_timer->setInterval(400);
    connect(m_timer, &QTimer::timeout, this, &RenderQueue::tick);
    load();
    startNext();
}

void RenderQueue::setStorePath(const QString& path) {
    m_storePath = path;
    load();
    startNext();
}

QString RenderQueue::resolvedStorePath() const {
    if (!m_storePath.isEmpty()) return m_storePath;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dir).filePath(QStringLiteral("renderqueue.json"));
}

int RenderQueue::countOfState(RenderJob::State s) const {
    int n = 0;
    for (const RenderJob& j : m_jobs) if (j.state == s) ++n;
    return n;
}

bool RenderQueue::hasActive() const {
    for (const RenderJob& j : m_jobs) if (j.state == RenderJob::Active) return true;
    return false;
}

QString RenderQueue::describeTarget(const OutputSettings& output) {
    const EncoderRegistry::Encoder* encoder = EncoderRegistry::find(output.videoCodec);
    const QString codec = encoder ? encoder->display : output.videoCodec;
    // Hardware encoders are driven by bitrate, software ones by CRF, so the
    // label quotes whichever number actually governs the encode.
    const QString rate = (encoder && encoder->isHardware)
                             ? QStringLiteral("%1 kb/s").arg(output.bitrateKbps)
                             : QStringLiteral("CRF %1").arg(output.crf);
    return QStringLiteral("%1x%2 %3fps %4 %5")
        .arg(output.width).arg(output.height).arg(output.fps).arg(codec, rate);
}

QString RenderQueue::enqueue(const RenderRequest& request, QString* error) {
    auto fail = [error](const QString& message) {
        if (error) *error = message;
        return QString();
    };

    if (request.timeline.isEmpty())
        return fail(QObject::tr("The timeline is empty, so there is nothing to render."));
    if (request.outputPath.isEmpty())
        return fail(QObject::tr("No output file was given."));

    const QFileInfo out(request.outputPath);
    if (out.isDir())
        return fail(QObject::tr("The output path is a folder; a file name is required."));
    const QDir parent = out.absoluteDir();
    if (!parent.exists())
        return fail(QObject::tr("The output folder does not exist: %1")
                        .arg(QDir::toNativeSeparators(parent.absolutePath())));

    RenderJob j;
    j.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    j.name = request.name.isEmpty() ? out.fileName() : request.name;
    j.project = request.project;
    j.projectPath = request.projectPath;
    j.outputPath = out.absoluteFilePath();
    j.output = request.output;
    j.timeline = request.timeline;   // snapshot: later edits do not reach this job
    j.target = describeTarget(request.output);
    j.state = RenderJob::Pending;
    m_jobs.push_back(j);
    save();
    if (error) error->clear();
    emit changed();
    startNext();
    return j.id;
}

void RenderQueue::retry(const QString& id) {
    for (RenderJob& j : m_jobs) {
        if (j.id == id && j.state == RenderJob::Failed) {
            // Retrying a job that never recorded what it renders would just put
            // an unrunnable job back in the queue.
            if (!j.renderable()) {
                j.error = QObject::tr("Job predates render settings and cannot be "
                                      "rendered. Enqueue it again.");
                emit changed();
                return;
            }
            j.state = RenderJob::Pending;
            j.progress = 0;
            j.error.clear();
            save();
            emit changed();
            startNext();
            return;
        }
    }
}

void RenderQueue::cancel(const QString& id) {
    for (int i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs[i].id == id
            && (m_jobs[i].state == RenderJob::Active || m_jobs[i].state == RenderJob::Pending)) {
            m_jobs.remove(i);
            save();
            emit changed();
            startNext();
            return;
        }
    }
}

void RenderQueue::clearCompleted() {
    const auto before = m_jobs.size();
    m_jobs.erase(std::remove_if(m_jobs.begin(), m_jobs.end(),
                 [](const RenderJob& j) { return j.state == RenderJob::Completed; }),
                 m_jobs.end());
    if (m_jobs.size() != before) { save(); emit changed(); }
}

void RenderQueue::setPaused(bool paused) {
    if (m_paused == paused) return;
    m_paused = paused;
    if (m_paused) m_timer->stop();
    else startNext();
    emit changed();
}

void RenderQueue::startNext() {
    if (m_paused || hasActive()) {
        if (hasActive() && !m_timer->isActive()) m_timer->start();
        return;
    }
    for (RenderJob& j : m_jobs) {
        if (j.state == RenderJob::Pending) {
            j.state = RenderJob::Active;
            j.progress = 0;
            emit changed();
            m_timer->start();
            return;
        }
    }
    m_timer->stop();
}

void RenderQueue::tick() {
    RenderJob* active = nullptr;
    for (RenderJob& j : m_jobs)
        if (j.state == RenderJob::Active) { active = &j; break; }
    if (!active) { m_timer->stop(); return; }

    active->progress += 3 + int(QRandomGenerator::global()->generateDouble() * 7);
    if (active->progress >= 100) {
        active->progress = 100;
        active->state = RenderJob::Completed;
        active->finishedAt = QDateTime::currentDateTime();
        save();
        emit changed();
        startNext();   // promote the next pending job
    } else {
        emit changed();
    }
}

void RenderQueue::load() {
    m_jobs.clear();
    QFile f(resolvedStorePath());
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        // Schema 0 was a bare array of jobs; schema 1 wraps them so the file can
        // say what it is.
        const QJsonArray arr = doc.isArray()
                                   ? doc.array()
                                   : doc.object().value(QStringLiteral("jobs")).toArray();
        for (const auto& v : arr) {
            RenderJob j = RenderJob::fromJson(v.toObject());
            if (j.state == RenderJob::Active) {   // no worker resumes, so requeue
                j.state = RenderJob::Pending;
                j.progress = 0;
            }
            // A job queued before jobs recorded what they render cannot be run:
            // retire it visibly rather than starting a render of nothing.
            if ((j.state == RenderJob::Pending) && !j.renderable()) {
                j.state = RenderJob::Failed;
                j.progress = 0;
                j.error = QObject::tr("Job predates render settings and cannot be "
                                      "rendered. Enqueue it again.");
            }
            m_jobs.push_back(j);
        }
    }
    emit changed();
}

void RenderQueue::save() const {
    const QString path = resolvedStorePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QJsonArray arr;
    for (const RenderJob& j : m_jobs) arr.append(j.toJson());
    QJsonObject root;
    root.insert(QStringLiteral("schema"), kStoreSchema);
    root.insert(QStringLiteral("jobs"), arr);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.commit();
}
