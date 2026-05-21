#include "project/ProjectRegistry.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>

QString ProjectInfo::sizeText() const {
    const double mb = sizeBytes / (1024.0 * 1024.0);
    if (mb >= 1024.0) return QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 1);
    if (mb >= 1.0)    return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
    return QStringLiteral("%1 KB").arg(sizeBytes / 1024.0, 0, 'f', 0);
}

namespace {
int peekSceneCount(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return -1;
    const QJsonDocument doc = QJsonDocument::fromJson(f.read(1 << 20));  // cap at 1 MB
    if (!doc.isObject()) return -1;
    const QJsonValue scenes = doc.object().value(QStringLiteral("scenes"));
    return scenes.isArray() ? scenes.toArray().size() : -1;
}

QString displayName(const QFileInfo& fi) {
    QString n = fi.fileName();
    if (n.endsWith(QStringLiteral(".malloy.json"), Qt::CaseInsensitive))
        n.chop(int(sizeof(".malloy.json")) - 1);
    else
        n = fi.completeBaseName();
    return n;
}
} // namespace

ProjectRegistry::ProjectRegistry(QObject* parent) : QObject(parent) {
    loadDirs();
    // Seed with common locations on first use.
    for (auto loc : {QStandardPaths::MoviesLocation, QStandardPaths::DocumentsLocation}) {
        const QString d = QStandardPaths::writableLocation(loc);
        if (!d.isEmpty() && !m_dirs.contains(d)) m_dirs << d;
    }
    rescan();
}

void ProjectRegistry::setSearchDirs(const QStringList& dirs) {
    m_persist = false;
    m_dirs = dirs;
    rescan();
}

void ProjectRegistry::addSearchDir(const QString& dir) {
    if (dir.isEmpty() || m_dirs.contains(dir)) { rescan(); return; }
    m_dirs << dir;
    if (m_persist) saveDirs();
    rescan();
}

void ProjectRegistry::loadDirs() {
    if (!m_persist) return;
    QSettings s;
    m_dirs = s.value(QStringLiteral("projects/searchDirs")).toStringList();
}

void ProjectRegistry::saveDirs() const {
    QSettings s;
    s.setValue(QStringLiteral("projects/searchDirs"), m_dirs);
}

void ProjectRegistry::rescan() {
    m_projects.clear();
    QSet<QString> seen;
    for (const QString& dir : m_dirs) {
        QDir d(dir);
        const auto entries = d.entryInfoList({QStringLiteral("*.malloy.json")},
                                             QDir::Files, QDir::Time);
        for (const QFileInfo& fi : entries) {
            const QString canon = fi.canonicalFilePath();
            if (canon.isEmpty() || seen.contains(canon)) continue;
            seen.insert(canon);
            ProjectInfo p;
            p.filePath   = fi.absoluteFilePath();
            p.name       = displayName(fi);
            p.modified   = fi.lastModified();
            p.sizeBytes  = fi.size();
            p.sceneCount = peekSceneCount(fi.absoluteFilePath());
            m_projects.push_back(p);
        }
    }
    std::sort(m_projects.begin(), m_projects.end(),
              [](const ProjectInfo& a, const ProjectInfo& b) { return a.modified > b.modified; });
    emit changed();
}
