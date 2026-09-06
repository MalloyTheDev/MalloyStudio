#pragma once

// RecentRecordings - the newest capture files in the recording output folder.
//
// Recordings are written wherever the user last saved (MainWindow persists
// that folder as QSettings "recording/lastDir"), so unlike clips there is no
// sidecar index to read back: the folder is listed on demand. The filtering
// and ordering rules live here, apart from the Dashboard widget, so they can
// be unit-tested.

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

struct RecordingInfo {
    QString   filePath;
    QString   name;
    qint64    sizeBytes = 0;
    QDateTime modified;

    QString sizeText() const;

    // Coarse "when" label for list rows: "Just now", "12 min ago", "3 hr ago",
    // "Yesterday", a weekday within the last week, then a short date.
    // `now` is injectable so the mapping can be tested without waiting.
    QString relativeTimeText(const QDateTime& now = QDateTime::currentDateTime()) const;
};

namespace RecentRecordings {

// Container extensions the app can produce, lower case and without the dot.
const QStringList& extensions();

// Folder to list: QSettings "recording/lastDir", falling back to the user's
// Movies location. May not exist yet on a fresh install.
QString outputDir();

// Newest-first recordings in `dir`; limit <= 0 returns every match.
// A missing or unreadable directory yields an empty list, not an error:
// an empty dashboard panel is the correct presentation either way.
QVector<RecordingInfo> scan(const QString& dir, int limit = 4);

} // namespace RecentRecordings
