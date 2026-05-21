#pragma once

// MediaRegistry — index of media files (video/audio/image) across known
// folders, for the Media workspace. A lightweight filesystem scan by
// extension; size/type/modified now. Duration/resolution via ffprobe is a
// deliberate follow-up (needs a subprocess), so those read "—" for now.

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

struct MediaInfo {
    enum Kind { Video, Audio, Image, Other };

    QString   filePath;
    QString   name;
    Kind      kind = Other;
    QString   ext;
    qint64    sizeBytes = 0;
    QDateTime modified;

    QString sizeText() const;
    QString kindText() const;
    QString iconName() const;

    static Kind kindForExt(const QString& lowerExt);
};

class MediaRegistry : public QObject {
    Q_OBJECT
public:
    explicit MediaRegistry(QObject* parent = nullptr);

    void setSearchDirs(const QStringList& dirs);   // tests: settings-free
    void addSearchDir(const QString& dir);         // persists + rescans
    QStringList searchDirs() const { return m_dirs; }

    const QVector<MediaInfo>& media() const { return m_media; }
    int count() const { return m_media.size(); }
    int countOfKind(MediaInfo::Kind kind) const;

    void rescan();

signals:
    void changed();

private:
    void loadDirs();
    void saveDirs() const;

    QStringList m_dirs;
    QVector<MediaInfo> m_media;
    bool m_persist = true;
};
