#pragma once

// MediaRegistry — index of media files (video/audio/image) across known
// folders, for the Media workspace. A lightweight filesystem scan by extension
// gives size/type/modified immediately; duration + resolution are filled in
// asynchronously via ffprobe (one subprocess per file), so the table stays
// responsive and degrades gracefully when ffprobe isn't installed.
//
// The folders themselves are listed off the GUI thread, each on its own, and
// changed() follows each answer. A folder can be on a network share that has
// gone away, where listing it waits for the redirector's timeout; that folder
// then answers late or as unavailable, and holds up neither the window nor the
// other folders.

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

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
    ~MediaRegistry() override;

    void setSearchDirs(const QStringList& dirs);   // tests: settings-free
    void addSearchDir(const QString& dir);         // persists; lists that folder again
    void removeSearchDir(const QString& dir);      // persists; its files leave at once
    QStringList searchDirs() const { return m_dirs; }
    // Folders whose latest listing found them missing or unreachable. They
    // contribute nothing until a later listing finds them.
    QStringList unavailableDirs() const;

    const QVector<MediaInfo>& media() const { return m_media; }
    int count() const { return m_media.size(); }
    int countOfKind(MediaInfo::Kind kind) const;

    void rescan();                                 // every folder, off the GUI thread
    bool scanning() const { return !m_inFlight.isEmpty(); }

    // Whether a file's data is somewhere else and reading it would fetch it:
    // a OneDrive or other cloud placeholder, or a file marked offline. Such a
    // file is listed but never probed, because opening it downloads it.
    static bool isCloudOnly(const QString& path);

    // What one ffprobe run (-print_format json -show_format -show_streams)
    // says about a file. The output describes the file being probed, so it is
    // whatever that file claims: a duration that is not a finite positive
    // number an int can hold is left unknown rather than narrowed.
    struct ProbeReport {
        int     durationSecs = 0;   // 0 = unknown
        QString resolution;         // first video stream, "W×H"; empty otherwise
    };
    static ProbeReport parseProbeOutput(const QByteArray& ffprobeJson);

    // ---- For tests ---------------------------------------------------------
    // Runs `program`, with `leadingArgs` before the usual ffprobe arguments,
    // in place of ffprobe, and gives each run `timeoutMs` before it is killed.
    void setProbeCommandForTesting(const QString& program, const QStringList& leadingArgs,
                                   int timeoutMs);
    // Where probe results are kept between launches.
    void setProbeCachePathForTesting(const QString& path);
    int  probesStarted() const { return m_probesStarted; }
    // A folder listing or a probe is still under way: the list can change.
    bool probing() const { return m_proc != nullptr || scanning(); }
    // Called on the listing thread with each folder, just before it is listed.
    void setFolderScanHookForTesting(std::function<void(const QString&)> hook);

signals:
    void changed();

private:
    struct FolderScan {
        struct Found {
            QString   canonicalPath;
            MediaInfo info;
            bool      cloudOnly = false;
        };
        bool           available = false;
        QVector<Found> found;
    };
    static FolderScan scanFolder(const QString& dir);   // runs off the GUI thread
    void scanFolders(const QStringList& dirs);
    void folderScanned(const QString& dir, const FolderScan& scan);
    void rebuild();   // m_media from the folders' answers; restarts probing
    void loadDirs();
    void saveDirs() const;
    void probeNext(int generation);   // async ffprobe walk
    void killProbe();                 // stop the in-flight ffprobe cleanly
    void emitChangedCoalesced();      // throttle probe-driven updates
    void finishProbe(int generation, int idx);  // record, then move to the next file
    void loadProbeCache();
    void saveProbeCache() const;
    QString probeCachePath() const;

    QStringList m_dirs;
    QVector<MediaInfo> m_media;
    // Canonical path of each entry of m_media, found while listing so the GUI
    // thread never has to ask the file system for it.
    QVector<QString> m_canonical;
    QHash<QString, FolderScan> m_scans;   // latest answer for each folder
    // One listing per folder at a time. A folder asked for again while its
    // listing is out is listed once more when that answers, so a stalled share
    // collects one waiting thread rather than one per recording.
    QSet<QString> m_inFlight;
    QSet<QString> m_again;
    std::function<void(const QString&)> m_scanHook;
    bool m_persist = true;

    // Absolute path to ffprobe, empty when it was not found. Resolved once at
    // construction so the launch below never depends on the process search
    // order; see the constructor.
    QString m_ffprobe;
    int  m_probeGen = 0;      // bumped each rescan to drop stale probe results
    int  m_probeIndex = 0;
    QProcess* m_proc = nullptr;   // the single in-flight ffprobe (chain is sequential)
    QTimer* m_coalesce = nullptr;

    // What earlier probes found, by canonical path, valid while the file's
    // size and modification time are unchanged. Kept across rescans and
    // launches: every launch and every finished recording used to probe the
    // newest two hundred files again from scratch.
    struct ProbeRecord {
        qint64    sizeBytes = 0;
        QDateTime modified;
        int       durationSecs = 0;
        QString   resolution;
    };
    QHash<QString, ProbeRecord> m_probeCache;
    bool m_probeCacheDirty = false;
    bool m_scanned = false;       // a scan has run; the deferred first one is then not needed
    QString m_probeCachePath;
    QStringList m_probeLeadingArgs;
    int m_probeTimeoutMs = 10000;
    int m_probesStarted = 0;
};
