#include "project/MediaRegistry.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>

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

MediaInfo::Kind MediaInfo::kindForExt(const QString& lowerExt) {
    if (videoExts().contains(lowerExt)) return Video;
    if (audioExts().contains(lowerExt)) return Audio;
    if (imageExts().contains(lowerExt)) return Image;
    return Other;
}

MediaRegistry::MediaRegistry(QObject* parent) : QObject(parent) {
    loadDirs();
    for (auto loc : {QStandardPaths::MoviesLocation, QStandardPaths::PicturesLocation,
                     QStandardPaths::MusicLocation}) {
        const QString d = QStandardPaths::writableLocation(loc);
        if (!d.isEmpty() && !m_dirs.contains(d)) m_dirs << d;
    }
    rescan();
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
}
