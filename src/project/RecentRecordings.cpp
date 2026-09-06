#include "project/RecentRecordings.h"
#include "project/ByteSize.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

QString RecordingInfo::sizeText() const {
    return formatByteSize(sizeBytes);
}

QString RecordingInfo::relativeTimeText(const QDateTime& now) const {
    if (!modified.isValid()) return QStringLiteral("Unknown");

    const qint64 secs = modified.secsTo(now);
    if (secs < 60) return QStringLiteral("Just now");          // also covers clock skew (negative secs)
    if (secs < 3600) return QStringLiteral("%1 min ago").arg(secs / 60);

    const QDate today = now.date();
    const QDate day   = modified.date();
    if (day == today) return QStringLiteral("%1 hr ago").arg(secs / 3600);
    if (day == today.addDays(-1)) return QStringLiteral("Yesterday");
    if (day > today.addDays(-7)) return day.toString(QStringLiteral("ddd"));
    if (day.year() == today.year()) return day.toString(QStringLiteral("MMM d"));
    return day.toString(QStringLiteral("MMM d yyyy"));
}

const QStringList& RecentRecordings::extensions() {
    // Video containers MediaController writes, plus the audio-only formats a
    // capture can land as. Kept lower case; QDir name filters are matched
    // case-insensitively on Windows.
    static const QStringList kExtensions = {
        QStringLiteral("mkv"), QStringLiteral("mp4"), QStringLiteral("mov"),
        QStringLiteral("flv"), QStringLiteral("ts"),  QStringLiteral("webm"),
        QStringLiteral("wav"), QStringLiteral("mp3"), QStringLiteral("m4a"),
        QStringLiteral("flac"),
    };
    return kExtensions;
}

QString RecentRecordings::outputDir() {
    QSettings settings;
    const QString fallback = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    return settings.value(QStringLiteral("recording/lastDir"), fallback).toString();
}

QVector<RecordingInfo> RecentRecordings::scan(const QString& dir, int limit) {
    QVector<RecordingInfo> out;
    if (dir.isEmpty()) return out;

    QDir d(dir);
    if (!d.exists()) return out;

    QStringList filters;
    filters.reserve(extensions().size());
    for (const QString& ext : extensions())
        filters << QStringLiteral("*.") + ext;

    const QFileInfoList entries =
        d.entryInfoList(filters, QDir::Files | QDir::Readable, QDir::Time);

    out.reserve(limit > 0 ? qMin(limit, static_cast<int>(entries.size())) : static_cast<int>(entries.size()));
    for (const QFileInfo& fi : entries) {
        if (limit > 0 && out.size() >= limit) break;
        RecordingInfo r;
        r.filePath  = fi.absoluteFilePath();
        r.name      = fi.fileName();
        r.sizeBytes = fi.size();
        r.modified  = fi.lastModified();
        out.push_back(r);
    }
    return out;
}
