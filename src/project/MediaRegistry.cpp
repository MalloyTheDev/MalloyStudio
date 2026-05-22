#include "project/MediaRegistry.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>

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
    const double mb = sizeBytes / (1024.0 * 1024.0);
    if (mb >= 1024.0) return QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 1);
    if (mb >= 1.0)    return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
    return QStringLiteral("%1 KB").arg(sizeBytes / 1024.0, 0, 'f', 0);
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
    m_ffprobe = !QStandardPaths::findExecutable(QStringLiteral("ffprobe")).isEmpty();
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
    rescan();
}

MediaRegistry::~MediaRegistry() {
    killProbe();
}

void MediaRegistry::killProbe() {
    if (!m_proc) return;
    // Drop the finished handler so it can't fire (and re-spawn the chain) while
    // we tear the process down, then stop and reap it.
    m_proc->disconnect(this);
    if (m_proc->state() != QProcess::NotRunning) {
        m_proc->kill();
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

void MediaRegistry::probeNext(int generation) {
    if (!m_ffprobe || generation != m_probeGen) return;
    while (m_probeIndex < m_media.size() && m_media[m_probeIndex].probed)
        ++m_probeIndex;
    // Cap probing so a huge media folder can't spawn an unbounded ffprobe chain.
    if (m_probeIndex >= m_media.size() || m_probeIndex >= 200) return;

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
            if (dur > 0) m.durationSecs = int(dur + 0.5);
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
        if (idx < m_media.size()) m_media[idx].probed = true;  // even on failure: don't retry
        emitChangedCoalesced();
        ++m_probeIndex;
        probeNext(generation);
    });
    proc->start(QStringLiteral("ffprobe"),
                {QStringLiteral("-v"), QStringLiteral("quiet"),
                 QStringLiteral("-print_format"), QStringLiteral("json"),
                 QStringLiteral("-show_format"), QStringLiteral("-show_streams"), path});
}
