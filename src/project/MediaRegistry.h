#pragma once

// MediaRegistry — index of media files (video/audio/image) across known
// folders, for the Media workspace. A lightweight filesystem scan by extension
// gives size/type/modified immediately; duration + resolution are filled in
// asynchronously via ffprobe (one subprocess per file), so the table stays
// responsive and degrades gracefully when ffprobe isn't installed.

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

class QProcess;
class QTimer;

struct MediaInfo {
    enum Kind { Video, Audio, Image, Other };

    QString   filePath;
    QString   name;
    Kind      kind = Other;
    QString   ext;
    qint64    sizeBytes = 0;
    QDateTime modified;
    int       durationSecs = 0;   // ffprobe; 0 = unknown
    QString   resolution;         // "1920×1080" for video; empty otherwise
    bool      probed = false;

    QString sizeText() const;
    QString kindText() const;
    QString iconName() const;
    QString durationText() const; // h:mm:ss / m:ss / "—"
    QString propsText() const;    // resolution + duration, falling back to kind·ext

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
    void probeNext(int generation);   // async ffprobe walk
    void emitChangedCoalesced();      // throttle probe-driven updates

    QStringList m_dirs;
    QVector<MediaInfo> m_media;
    bool m_persist = true;

    bool m_ffprobe = false;   // ffprobe available on PATH
    int  m_probeGen = 0;      // bumped each rescan to drop stale probe results
    int  m_probeIndex = 0;
    QTimer* m_coalesce = nullptr;
};
