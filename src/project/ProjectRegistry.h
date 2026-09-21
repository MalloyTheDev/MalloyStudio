#pragma once

// ProjectRegistry — index of .malloy.json projects across known folders
// (audit.html's ProjectRegistry). Scans a persisted set of search directories
// and exposes lightweight metadata (name, modified, size, scene count) for the
// Projects workspace. No project content is loaded — that stays in
// ProjectDocument when the user actually opens one.
//
// Folders are listed off the GUI thread, each on its own, and changed() follows
// each answer. A folder can be on a network share that has gone away, where
// listing it waits for the redirector's timeout; that folder then answers late
// or as unavailable, and holds up neither the window nor the other folders.

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

struct ProjectInfo {
    QString   filePath;
    QString   name;
    QDateTime modified;
    qint64    sizeBytes = 0;
    int       sceneCount = -1;   // -1 = unknown

    QString sizeText() const;
};

class ProjectRegistry : public QObject {
    Q_OBJECT
public:
    explicit ProjectRegistry(QObject* parent = nullptr);

    // Tests inject an isolated settings-free dir set instead of the defaults.
    void setSearchDirs(const QStringList& dirs);
    void addSearchDir(const QString& dir);     // persists; lists that folder again
    void removeSearchDir(const QString& dir);  // persists; its projects leave at once
    QStringList searchDirs() const { return m_dirs; }
    // Folders whose latest listing found them missing or unreachable. They
    // contribute nothing until a later listing finds them.
    QStringList unavailableDirs() const;

    const QVector<ProjectInfo>& projects() const { return m_projects; }
    int count() const { return m_projects.size(); }

    void rescan();                              // every folder, off the GUI thread
    bool scanning() const { return !m_inFlight.isEmpty(); }

    // ---- For tests ---------------------------------------------------------
    // Called on the listing thread with each folder, just before it is listed.
    void setFolderScanHookForTesting(std::function<void(const QString&)> hook);

signals:
    void changed();

private:
    struct FolderScan {
        struct Found {
            QString     canonicalPath;
            ProjectInfo info;
        };
        bool           available = false;
        QVector<Found> found;
    };
    static FolderScan scanFolder(const QString& dir);   // runs off the GUI thread
    void scanFolders(const QStringList& dirs);
    void folderScanned(const QString& dir, const FolderScan& scan);
    void rebuild();
    void loadDirs();
    void saveDirs() const;

    QStringList m_dirs;
    QVector<ProjectInfo> m_projects;
    QHash<QString, FolderScan> m_scans;   // latest answer for each folder
    // One listing per folder at a time. A folder asked for again while its
    // listing is out is listed once more when that answers, so a stalled share
    // collects one waiting thread rather than one per save.
    QSet<QString> m_inFlight;
    QSet<QString> m_again;
    std::function<void(const QString&)> m_scanHook;
    bool m_persist = true;   // false for tests using setSearchDirs
    bool m_scanned = false;  // a scan has run; the deferred first one is then not needed
};
