#include "project/MediaRegistry.h"
#include "project/ByteSize.h"
#include "platform/ProcessTree.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

#include <cmath>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QSaveFile>
#include <QTimeZone>
#include <QTimer>

#include <algorithm>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {
const QStringList& videoExts() {
    static const QStringList e = {QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("mov"),
                                  QStringLiteral("avi"), QStringLiteral("webm"), QStringLiteral("m4v")};
    return e;
}
const QStringList& audioExts() {
    static const QStringList e = {QStringLiteral("wav"), QStringLiteral("flac"), QStringLiteral("mp3"),
                                  QStringLiteral("aac"), QStringLiteral("m4a"), QStringLiteral("ogg"),
                                  QStringLiteral("opus")};
    return e;
}
const QStringList& imageExts() {
    static const QStringList e = {QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
                                  QStringLiteral("bmp"), QStringLiteral("gif"), QStringLiteral("webp"),
                                  QStringLiteral("psd")};
    return e;
}
} // namespace

QString MediaInfo::sizeText() const {
    return formatByteSize(sizeBytes);
}

QString MediaInfo::kindText() const {
    switch (kind) {
    case Video: return QStringLiteral("Video");
    case Audio: return QStringLiteral("Audio");
    case Image: return QStringLiteral("Image");
    default:    return QStringLiteral("File");
    }
}

QString MediaInfo::iconName() const {
    switch (kind) {
    case Video: return QStringLiteral("editor");
    case Audio: return QStringLiteral("speaker");
    case Image: return QStringLiteral("image");
    default:    return QStringLiteral("media");
    }
}

QString MediaInfo::durationText() const {
    if (durationSecs <= 0) return QStringLiteral("—");
    const int h = durationSecs / 3600, m = (durationSecs % 3600) / 60, s = durationSecs % 60;
    if (h > 0) return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
}

QString MediaInfo::propsText() const {
    QStringList parts;
    if (!resolution.isEmpty()) parts << resolution;
    if (durationSecs > 0)      parts << durationText();
    if (parts.isEmpty())       return QStringLiteral("%1 · %2").arg(kindText(), ext.toUpper());
    return parts.join(QStringLiteral(" · "));
}

MediaInfo::Kind MediaInfo::kindForExt(const QString& lowerExt) {
    if (videoExts().contains(lowerExt)) return Video;
    if (audioExts().contains(lowerExt)) return Audio;
    if (imageExts().contains(lowerExt)) return Image;
    return Other;
}

MediaRegistry::MediaRegistry(QObject* parent) : QObject(parent) {
    // Keep the resolved path, not just the fact that one was found. Starting a
    // bare name later hands the lookup to CreateProcess, whose search order
    // includes the application directory and the current directory, so a file
    // named ffprobe.exe dropped beside the program would run instead.
    m_ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    m_coalesce = new QTimer(this);
    m_coalesce->setSingleShot(true);
    m_coalesce->setInterval(250);
    connect(m_coalesce, &QTimer::timeout, this, &MediaRegistry::changed);

    loadDirs();
    for (auto loc : {QStandardPaths::MoviesLocation, QStandardPaths::PicturesLocation,
                     QStandardPaths::MusicLocation}) {
        const QString d = QStandardPaths::writableLocation(loc);
        if (!d.isEmpty() && !m_dirs.contains(d)) m_dirs << d;
    }
    loadProbeCache();
    // The first scan waits for the event loop rather than running inside the
    // constructor: it walks folders and starts probing, which is no part of
    // constructing the object, and a caller that sets its own folders first
    // (tests do) should not have the default ones scanned and probed.
    QTimer::singleShot(0, this, [this] { if (!m_scanned) rescan(); });
}

MediaRegistry::~MediaRegistry() {
    killProbe();
}

bool MediaRegistry::isCloudOnly(const QString& path) {
    // Reading the attributes does not fetch the file; opening it does.
    const DWORD attrs = GetFileAttributesW(reinterpret_cast<const wchar_t*>(
        QDir::toNativeSeparators(path).utf16()));
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    constexpr DWORD kRecallOnDataAccess = 0x00400000;   // FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
    constexpr DWORD kRecallOnOpen       = 0x00040000;   // FILE_ATTRIBUTE_RECALL_ON_OPEN
    return (attrs & (FILE_ATTRIBUTE_OFFLINE | kRecallOnDataAccess | kRecallOnOpen)) != 0;
}

void MediaRegistry::setProbeCommandForTesting(const QString& program,
                                              const QStringList& leadingArgs, int timeoutMs) {
    m_ffprobe = program;
    m_probeLeadingArgs = leadingArgs;
    m_probeTimeoutMs = timeoutMs;
}

void MediaRegistry::setProbeCachePathForTesting(const QString& path) {
    m_probeCachePath = path;
    m_probeCache.clear();
    loadProbeCache();
}

QString MediaRegistry::probeCachePath() const {
    if (!m_probeCachePath.isEmpty()) return m_probeCachePath;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return dir.isEmpty() ? QString() : QDir(dir).filePath(QStringLiteral("media-probe-cache.json"));
}

void MediaRegistry::loadProbeCache() {
    const QString path = probeCachePath();
    if (path.isEmpty()) return;
    QFile f(path);
    // A cache is only ever an optimisation: anything unreadable, oversized or
    // malformed is treated as no cache, and the files are simply probed.
    if (!f.open(QIODevice::ReadOnly) || f.size() > 8 * 1024 * 1024) return;
    const QJsonArray entries = QJsonDocument::fromJson(f.readAll()).object()
                                   .value(QStringLiteral("entries")).toArray();
    for (const QJsonValue& v : entries) {
        const QJsonObject o = v.toObject();
        const QString key = o.value(QStringLiteral("path")).toString();
        if (key.isEmpty()) continue;
        // Converting a double outside the integer's range is undefined, so the
        // numbers are checked before they are narrowed. The comparisons are
        // also false for NaN.
        const double size = o.value(QStringLiteral("size")).toDouble(-1);
        const double modified = o.value(QStringLiteral("modified")).toDouble(0);
        if (!(size >= 0 && size < 9e15) || !(modified > -9e15 && modified < 9e15)) continue;
        ProbeRecord r;
        r.sizeBytes = qint64(size);
        r.modified = QDateTime::fromMSecsSinceEpoch(qint64(modified), QTimeZone::UTC);
        const int dur = o.value(QStringLiteral("duration")).toInt(0);
        r.durationSecs = dur > 0 ? dur : 0;
        r.resolution = o.value(QStringLiteral("resolution")).toString().left(32);
        m_probeCache.insert(key, r);
    }
}

void MediaRegistry::saveProbeCache() const {
    const QString path = probeCachePath();
    if (path.isEmpty()) return;
    // Only what the current scan holds, so the cache is bounded by the folders
    // being indexed rather than growing with every file ever seen.
    QJsonArray entries;
    for (const MediaInfo& m : m_media) {
        const QString key = QFileInfo(m.filePath).canonicalFilePath();
        const auto it = m_probeCache.constFind(key);
        if (it == m_probeCache.constEnd()) continue;
        entries.append(QJsonObject{
            {QStringLiteral("path"), key},
            {QStringLiteral("size"), double(it->sizeBytes)},
            {QStringLiteral("modified"), double(it->modified.toMSecsSinceEpoch())},
            {QStringLiteral("duration"), it->durationSecs},
            {QStringLiteral("resolution"), it->resolution},
        });
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(QJsonObject{{QStringLiteral("entries"), entries}}).toJson(QJsonDocument::Compact));
    f.commit();
}

void MediaRegistry::killProbe() {
    if (!m_proc) return;
    // Drop the finished handler so it can't fire (and re-spawn the chain) while
    // we tear the process down, then stop and reap it.
    m_proc->disconnect(this);
    if (m_proc->state() != QProcess::NotRunning) {
        ProcessTree::kill(quint32(m_proc->processId()));   // see ProcessTree
        m_proc->waitForFinished(1000);
    }
    m_proc->deleteLater();
    m_proc = nullptr;
}

void MediaRegistry::emitChangedCoalesced() {
    if (m_coalesce && !m_coalesce->isActive()) m_coalesce->start();
}

void MediaRegistry::setSearchDirs(const QStringList& dirs) {
    m_persist = false;
    m_dirs = dirs;
    rescan();
}

void MediaRegistry::addSearchDir(const QString& dir) {
    if (dir.isEmpty() || m_dirs.contains(dir)) { rescan(); return; }
    m_dirs << dir;
    if (m_persist) saveDirs();
    rescan();
}

void MediaRegistry::loadDirs() {
    if (!m_persist) return;
    QSettings s;
    m_dirs = s.value(QStringLiteral("media/searchDirs")).toStringList();
}

void MediaRegistry::saveDirs() const {
    QSettings s;
    s.setValue(QStringLiteral("media/searchDirs"), m_dirs);
}

int MediaRegistry::countOfKind(MediaInfo::Kind kind) const {
    int n = 0;
    for (const MediaInfo& m : m_media) if (m.kind == kind) ++n;
    return n;
}

void MediaRegistry::rescan() {
    m_scanned = true;
    m_media.clear();

    QStringList filters;
    for (const QStringList* set : {&videoExts(), &audioExts(), &imageExts()})
        for (const QString& e : *set) filters << QStringLiteral("*.%1").arg(e);

    QSet<QString> seen;
    for (const QString& dir : m_dirs) {
        QDir d(dir);
        const auto entries = d.entryInfoList(filters, QDir::Files, QDir::Time);
        for (const QFileInfo& fi : entries) {
            const QString canon = fi.canonicalFilePath();
            if (canon.isEmpty() || seen.contains(canon)) continue;
            seen.insert(canon);
            MediaInfo m;
            m.filePath  = fi.absoluteFilePath();
            m.name      = fi.fileName();
            m.ext       = fi.suffix().toLower();
            m.kind      = MediaInfo::kindForExt(m.ext);
            m.sizeBytes = fi.size();
            m.modified  = fi.lastModified();
            const auto cached = m_probeCache.constFind(canon);
            if (cached != m_probeCache.constEnd() && cached->sizeBytes == m.sizeBytes
                && cached->modified == m.modified) {
                m.durationSecs = cached->durationSecs;
                m.resolution   = cached->resolution;
                m.probed       = true;
            } else if (isCloudOnly(canon)) {
                m.probed = true;   // listed, never opened; see isCloudOnly
            }
            m_media.push_back(m);
        }
    }
    std::sort(m_media.begin(), m_media.end(),
              [](const MediaInfo& a, const MediaInfo& b) { return a.modified > b.modified; });
    emit changed();

    // Kick off async metadata probing for this scan generation. Stop any
    // in-flight probe from the previous generation first, so we never run two
    // ffprobe chains at once nor leak one when a rescan supersedes it.
    killProbe();
    ++m_probeGen;
    m_probeIndex = 0;
    probeNext(m_probeGen);
}

void MediaRegistry::finishProbe(int generation, int idx) {
    if (generation != m_probeGen) return;
    if (idx < m_media.size()) {
        MediaInfo& m = m_media[idx];
        m.probed = true;   // even on failure: don't retry
        // Failures are remembered too, so a file ffprobe cannot read is not
        // opened again at every launch.
        const QString key = QFileInfo(m.filePath).canonicalFilePath();
        if (!key.isEmpty()) {
            m_probeCache.insert(key, ProbeRecord{m.sizeBytes, m.modified, m.durationSecs, m.resolution});
            m_probeCacheDirty = true;
        }
    }
    emitChangedCoalesced();
    ++m_probeIndex;
    probeNext(generation);
}

void MediaRegistry::probeNext(int generation) {
    if (m_ffprobe.isEmpty() || generation != m_probeGen) return;
    while (m_probeIndex < m_media.size() && m_media[m_probeIndex].probed)
        ++m_probeIndex;
    // Cap probing so a huge media folder can't spawn an unbounded ffprobe chain.
    if (m_probeIndex >= m_media.size() || m_probeIndex >= 200) {
        if (m_probeCacheDirty) {
            saveProbeCache();
            m_probeCacheDirty = false;
        }
        return;
    }

    const int idx = m_probeIndex;
    const QString path = m_media[idx].filePath;
    auto* proc = new QProcess(this);
    m_proc = proc;
    connect(proc, &QProcess::finished, this,
            [this, proc, generation, idx](int code, QProcess::ExitStatus status) {
        const QByteArray out = proc->readAllStandardOutput();
        if (m_proc == proc) m_proc = nullptr;
        proc->deleteLater();
        if (generation != m_probeGen) return;        // a newer rescan superseded us
        if (status == QProcess::NormalExit && code == 0 && idx < m_media.size()) {
            const QJsonObject root = QJsonDocument::fromJson(out).object();
            MediaInfo& m = m_media[idx];
            const double dur = root.value(QStringLiteral("format")).toObject()
                                   .value(QStringLiteral("duration")).toString().toDouble();
            // Comes from the file being probed, so it is whatever that file
            // says. A conversion to int is undefined once the double is larger
            // than int can hold, so the value is bounded before it is narrowed
            // rather than trusted to be a plausible duration.
            if (std::isfinite(dur) && dur > 0 && dur < 2147483647.0)
                m.durationSecs = int(dur + 0.5);
            for (const QJsonValue& sv : root.value(QStringLiteral("streams")).toArray()) {
                const QJsonObject s = sv.toObject();
                if (s.value(QStringLiteral("codec_type")).toString() == QLatin1String("video")) {
                    const int w = s.value(QStringLiteral("width")).toInt();
                    const int h = s.value(QStringLiteral("height")).toInt();
                    if (w > 0 && h > 0) m.resolution = QStringLiteral("%1×%2").arg(w).arg(h);
                    break;
                }
            }
        }
        finishProbe(generation, idx);
    });
    // A probe that cannot start never finishes, and one that hangs (a file on
    // a stalled network share, a pathological file) never finishes either;
    // both used to end the chain there, leaving every later file unprobed.
    connect(proc, &QProcess::errorOccurred, this,
            [this, proc, generation, idx](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;   // the others still finish
        if (m_proc == proc) m_proc = nullptr;
        proc->deleteLater();
        finishProbe(generation, idx);
    });
    QTimer::singleShot(m_probeTimeoutMs, proc, [proc] {
        // The whole tree, or a launcher's real ffprobe stays hung; see
        // ProcessTree. finished follows.
        if (proc->state() != QProcess::NotRunning) ProcessTree::kill(quint32(proc->processId()));
    });
    ++m_probesStarted;
    proc->start(m_ffprobe,
                m_probeLeadingArgs + QStringList{
                    QStringLiteral("-v"), QStringLiteral("quiet"),
                    QStringLiteral("-print_format"), QStringLiteral("json"),
                    QStringLiteral("-show_format"), QStringLiteral("-show_streams"), path});
}
