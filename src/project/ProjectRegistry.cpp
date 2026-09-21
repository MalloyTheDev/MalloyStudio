#include "project/ProjectRegistry.h"
#include "project/ProjectDocument.h"
#include "platform/OffThread.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

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
    return ProjectDocument::displayName(fi.absoluteFilePath());
}
} // namespace

ProjectRegistry::ProjectRegistry(QObject* parent) : QObject(parent) {
    loadDirs();
    // The first scan waits for the event loop, as MediaRegistry's does: a
    // caller that sets its own folders first (tests do) should not have the
    // user's Movies and Documents read on its behalf.
    QTimer::singleShot(0, this, [this] { if (!m_scanned) rescan(); });
}

void ProjectRegistry::setSearchDirs(const QStringList& dirs) {
    m_persist = false;
    m_dirs = dirs;
    rescan();
}

void ProjectRegistry::addSearchDir(const QString& dir) {
    if (dir.isEmpty()) return;
    if (!m_dirs.contains(dir)) {
        m_dirs << dir;
        if (m_persist) saveDirs();
    }
    // Only this folder, where a project was just saved or opened. The others
    // have not changed because of it, and one of them may be a share that
    // takes a long time to answer.
    scanFolders({dir});
}

void ProjectRegistry::removeSearchDir(const QString& dir) {
    if (m_dirs.removeAll(dir) == 0) return;
    if (m_persist) saveDirs();
    m_scans.remove(dir);
    m_again.remove(dir);
    rebuild();
}

QStringList ProjectRegistry::unavailableDirs() const {
    QStringList dirs;
    for (const QString& dir : m_dirs) {
        const auto it = m_scans.constFind(dir);
        if (it != m_scans.constEnd() && !it->available) dirs << dir;
    }
    return dirs;
}

void ProjectRegistry::setFolderScanHookForTesting(std::function<void(const QString&)> hook) {
    m_scanHook = std::move(hook);
}

void ProjectRegistry::loadDirs() {
    if (!m_persist) return;
    QSettings s;
    const QString key = QStringLiteral("projects/searchDirs");
    // A saved list is the user's own, defaults included, so a default folder
    // they removed stays removed. Without one, common locations seed it.
    if (s.contains(key)) {
        m_dirs = s.value(key).toStringList();
        return;
    }
    for (auto loc : {QStandardPaths::MoviesLocation, QStandardPaths::DocumentsLocation}) {
        const QString d = QStandardPaths::writableLocation(loc);
        if (!d.isEmpty() && !m_dirs.contains(d)) m_dirs << d;
    }
}

void ProjectRegistry::saveDirs() const {
    QSettings s;
    s.setValue(QStringLiteral("projects/searchDirs"), m_dirs);
}

void ProjectRegistry::rescan() {
    m_scanned = true;
    // Answers for folders no longer searched go now, rather than lingering
    // until they would have been replaced.
    bool dropped = false;
    for (auto it = m_scans.begin(); it != m_scans.end();) {
        if (m_dirs.contains(it.key())) {
            ++it;
        } else {
            it = m_scans.erase(it);
            dropped = true;
        }
    }
    if (dropped) rebuild();
    scanFolders(m_dirs);
}

void ProjectRegistry::scanFolders(const QStringList& dirs) {
    for (const QString& dir : dirs) {
        if (m_inFlight.contains(dir)) {
            m_again.insert(dir);
            continue;
        }
        m_inFlight.insert(dir);
        OffThread::run(this,
            [dir, hook = m_scanHook] {
                if (hook) hook(dir);
                return scanFolder(dir);
            },
            [this, dir](const FolderScan& scan) { folderScanned(dir, scan); });
    }
}

void ProjectRegistry::folderScanned(const QString& dir, const FolderScan& scan) {
    m_inFlight.remove(dir);
    const bool searched = m_dirs.contains(dir);   // it may have been removed meanwhile
    if (searched) m_scans.insert(dir, scan);
    if (m_again.remove(dir) && searched) scanFolders({dir});
    if (searched) rebuild();
}

ProjectRegistry::FolderScan ProjectRegistry::scanFolder(const QString& dir) {
    FolderScan scan;
    const QDir d(dir);
    // On a share that has gone away, this is the call that waits.
    if (dir.isEmpty() || !d.exists()) return scan;
    scan.available = true;
    const auto entries = d.entryInfoList({QStringLiteral("*.malloy.json")}, QDir::Files, QDir::Time);
    for (const QFileInfo& fi : entries) {
        FolderScan::Found f;
        f.canonicalPath = fi.canonicalFilePath();
        if (f.canonicalPath.isEmpty()) continue;
        f.info.filePath   = fi.absoluteFilePath();
        f.info.name       = displayName(fi);
        f.info.modified   = fi.lastModified();
        f.info.sizeBytes  = fi.size();
        f.info.sceneCount = peekSceneCount(fi.absoluteFilePath());
        scan.found.push_back(f);
    }
    return scan;
}

void ProjectRegistry::rebuild() {
    m_projects.clear();
    // One file reached through two folders (a junction, or a mapped drive
    // and its UNC path) is listed once.
    QSet<QString> seen;
    for (const QString& dir : m_dirs) {
        const auto it = m_scans.constFind(dir);
        if (it == m_scans.constEnd()) continue;
        for (const FolderScan::Found& f : it->found) {
            if (seen.contains(f.canonicalPath)) continue;
            seen.insert(f.canonicalPath);
            m_projects.push_back(f.info);
        }
    }
    std::sort(m_projects.begin(), m_projects.end(),
              [](const ProjectInfo& a, const ProjectInfo& b) { return a.modified > b.modified; });
    emit changed();
}
