#pragma once

// ClipsRegistry — sidecar index of replay-buffer clips (audit.html's
// ClipsRegistry). Persists clip metadata (name, source project, recorded date,
// size, duration, tags, favorite) as JSON in the app data location so the
// Clips workspace can show real captures across runs.
//
// The store is shared by every window of the application, and each keeps the
// list it read. A save therefore re-reads the store under a lock file and lays
// only this instance's own changes over it, so one window never writes away
// the clips another added. A store that cannot be parsed is moved aside, never
// written over, and one that cannot be read at all is left alone.

#include <QDateTime>
#include <QObject>
#include <QSet>
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
    // Uses `storePath` from the start, so the app-data store is never read
    // (tests). Empty => default app-data location.
    explicit ClipsRegistry(const QString& storePath, QObject* parent = nullptr);

    // Custom store path (used by tests). Empty => default app-data location.
    void setStorePath(const QString& path);

    const QVector<ClipInfo>& clips() const { return m_clips; }
    int count() const { return m_clips.size(); }

    void addClip(const ClipInfo& clip);     // prepend, persist, emit changed
    // Convenience for a freshly saved file: probes size, fills metadata.
    ClipInfo registerFile(const QString& filePath, const QString& sourceProject, int durationSecs);
    void setFavorite(const QString& id, bool favorite);

    // Re-reads the store. Changes this instance has not managed to save yet
    // are kept, laid over what the store holds.
    void load();
    // Writes this instance's unsaved changes into the store, merged with what
    // the store holds now. False when they could not be written, after
    // saveFailed says why; they are kept and go out with the next save.
    bool save();

signals:
    void changed();
    void saveFailed(const QString& reason);

private:
    enum class StoreRead { Missing, Unreadable, Unparseable, Read };
    static StoreRead readStore(const QString& path, QVector<ClipInfo>* clips, QString* error);
    QVector<ClipInfo> withUnsavedChanges(QVector<ClipInfo> stored) const;
    bool persist();
    QString resolvedStorePath() const;

    QVector<ClipInfo> m_clips;
    QString m_storePath;   // override; empty => default
    // Ids of the entries added or edited here and not yet written. For every
    // other entry the store's own version wins, so a save never undoes what
    // another instance wrote in the meantime.
    QSet<QString> m_unsaved;
};
