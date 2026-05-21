#include "recording/RenderQueue.h"

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
    o.insert(QStringLiteral("target"), target);
    o.insert(QStringLiteral("outputPath"), outputPath);
    o.insert(QStringLiteral("error"), error);
    o.insert(QStringLiteral("state"), int(state));
    o.insert(QStringLiteral("progress"), progress);
    o.insert(QStringLiteral("finishedAt"), finishedAt.toString(Qt::ISODate));
    return o;
}

RenderJob RenderJob::fromJson(const QJsonObject& o) {
    RenderJob j;
    j.id         = o.value(QStringLiteral("id")).toString();
    j.name       = o.value(QStringLiteral("name")).toString();
    j.project    = o.value(QStringLiteral("project")).toString();
    j.target     = o.value(QStringLiteral("target")).toString();
    j.outputPath = o.value(QStringLiteral("outputPath")).toString();
    j.error      = o.value(QStringLiteral("error")).toString();
    j.state      = static_cast<State>(o.value(QStringLiteral("state")).toInt());
    j.progress   = o.value(QStringLiteral("progress")).toInt();
    j.finishedAt = QDateTime::fromString(o.value(QStringLiteral("finishedAt")).toString(), Qt::ISODate);
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

QString RenderQueue::enqueue(const QString& name, const QString& target,
                             const QString& project, const QString& outputPath) {
    RenderJob j;
    j.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    j.name = name;
    j.target = target;
    j.project = project;
    j.outputPath = outputPath;
    j.state = RenderJob::Pending;
    m_jobs.push_back(j);
    save();
    emit changed();
    startNext();
    return j.id;
}

void RenderQueue::retry(const QString& id) {
    for (RenderJob& j : m_jobs) {
        if (j.id == id && j.state == RenderJob::Failed) {
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
        const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).array();
        for (const auto& v : arr) {
            RenderJob j = RenderJob::fromJson(v.toObject());
            if (j.state == RenderJob::Active) {   // no worker resumes — requeue
                j.state = RenderJob::Pending;
                j.progress = 0;
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
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
    f.commit();
}
