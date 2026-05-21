#pragma once

// ProjectRegistry — index of .malloy.json projects across known folders
// (audit.html's ProjectRegistry). Scans a persisted set of search directories
// and exposes lightweight metadata (name, modified, size, scene count) for the
// Projects workspace. No project content is loaded — that stays in
// ProjectDocument when the user actually opens one.

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

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
    void addSearchDir(const QString& dir);     // persists + rescans
    QStringList searchDirs() const { return m_dirs; }

    const QVector<ProjectInfo>& projects() const { return m_projects; }
    int count() const { return m_projects.size(); }

    void rescan();

signals:
    void changed();

private:
    void loadDirs();
    void saveDirs() const;

    QStringList m_dirs;
    QVector<ProjectInfo> m_projects;
    bool m_persist = true;   // false for tests using setSearchDirs
};
