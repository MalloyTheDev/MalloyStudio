#pragma once

// ClipsRegistry — sidecar index of replay-buffer clips (audit.html's
// ClipsRegistry). Persists clip metadata (name, source project, recorded date,
// size, duration, tags, favorite) as JSON in the app data location so the
// Clips workspace can show real captures across runs.

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

class QJsonObject;

struct ClipInfo {
    QString     id;
    QString     filePath;
    QString     name;
    QString     sourceProject;
    QDateTime   recordedAt;
    qint64      sizeBytes = 0;
    int         durationSecs = 0;
    QStringList tags;
    bool        favorite = false;
    bool        archived = false;

    QJsonObject toJson() const;
    static ClipInfo fromJson(const QJsonObject& o);

    QString durationText() const;   // m:ss
    QString sizeText() const;       // human-readable
};

class ClipsRegistry : public QObject {
    Q_OBJECT
public:
    explicit ClipsRegistry(QObject* parent = nullptr);

    // Custom store path (used by tests). Empty => default app-data location.
    void setStorePath(const QString& path);

    const QVector<ClipInfo>& clips() const { return m_clips; }
    int count() const { return m_clips.size(); }

    void addClip(const ClipInfo& clip);     // prepend, persist, emit changed
    // Convenience for a freshly saved file: probes size, fills metadata.
    ClipInfo registerFile(const QString& filePath, const QString& sourceProject, int durationSecs);
    void setFavorite(const QString& id, bool favorite);

    void load();
    void save() const;

signals:
    void changed();

private:
    QString resolvedStorePath() const;

    QVector<ClipInfo> m_clips;
    QString m_storePath;   // override; empty => default
};
