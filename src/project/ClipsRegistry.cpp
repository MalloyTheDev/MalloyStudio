#include "project/ClipsRegistry.h"
#include "project/ByteSize.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

QJsonObject ClipInfo::toJson() const {
    QJsonObject o;
    o.insert(QStringLiteral("id"), id);
    o.insert(QStringLiteral("filePath"), filePath);
    o.insert(QStringLiteral("name"), name);
    o.insert(QStringLiteral("sourceProject"), sourceProject);
    o.insert(QStringLiteral("recordedAt"), recordedAt.toString(Qt::ISODate));
    o.insert(QStringLiteral("sizeBytes"), double(sizeBytes));
    o.insert(QStringLiteral("durationSecs"), durationSecs);
    o.insert(QStringLiteral("tags"), QJsonArray::fromStringList(tags));
    o.insert(QStringLiteral("favorite"), favorite);
    o.insert(QStringLiteral("archived"), archived);
    return o;
}

ClipInfo ClipInfo::fromJson(const QJsonObject& o) {
    ClipInfo c;
    c.id            = o.value(QStringLiteral("id")).toString();
    c.filePath      = o.value(QStringLiteral("filePath")).toString();
    c.name          = o.value(QStringLiteral("name")).toString();
    c.sourceProject = o.value(QStringLiteral("sourceProject")).toString();
    c.recordedAt    = QDateTime::fromString(o.value(QStringLiteral("recordedAt")).toString(), Qt::ISODate);
    c.sizeBytes     = qint64(o.value(QStringLiteral("sizeBytes")).toDouble());
    c.durationSecs  = o.value(QStringLiteral("durationSecs")).toInt();
    const QJsonArray tags = o.value(QStringLiteral("tags")).toArray();
    for (const auto& t : tags) c.tags << t.toString();
    c.favorite      = o.value(QStringLiteral("favorite")).toBool();
    c.archived      = o.value(QStringLiteral("archived")).toBool();
    return c;
}

QString ClipInfo::durationText() const {
    if (durationSecs <= 0) return QStringLiteral("—");
    return QStringLiteral("%1:%2").arg(durationSecs / 60).arg(durationSecs % 60, 2, 10, QChar('0'));
}

QString ClipInfo::sizeText() const {
    return formatByteSize(sizeBytes);
}

ClipsRegistry::ClipsRegistry(QObject* parent) : QObject(parent) {
    load();
}

ClipsRegistry::ClipsRegistry(const QString& storePath, QObject* parent)
    : QObject(parent), m_storePath(storePath) {
    load();
}

void ClipsRegistry::setStorePath(const QString& path) {
    m_storePath = path;
    m_unsaved.clear();   // they belong to the store being left
    load();
}

QString ClipsRegistry::resolvedStorePath() const {
    if (!m_storePath.isEmpty()) return m_storePath;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dir).filePath(QStringLiteral("clips.json"));
}

namespace {
// Held only while the store is read or rewritten, a few milliseconds, so a
// wait longer than this means the holder is stuck rather than busy.
constexpr int kLockWaitMs = 1000;

QString lockPathFor(const QString& storePath) {
    return storePath + QStringLiteral(".lock");
}
} // namespace

ClipsRegistry::StoreRead ClipsRegistry::readStore(const QString& path, QVector<ClipInfo>* clips,
                                                  QString* error) {
    QFile f(path);
    if (!f.exists()) return StoreRead::Missing;
    if (!f.open(QIODevice::ReadOnly)) {
        *error = f.errorString();
        return StoreRead::Unreadable;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        *error = parseError.errorString();
        return StoreRead::Unparseable;
    }
    QJsonArray arr;
    if (doc.isArray()) {
        arr = doc.array();
    } else if (doc.object().value(QStringLiteral("clips")).isArray()) {
        arr = doc.object().value(QStringLiteral("clips")).toArray();
    } else {
        *error = QStringLiteral("not a list of clips");
        return StoreRead::Unparseable;
    }
    for (const auto& v : arr) clips->push_back(ClipInfo::fromJson(v.toObject()));
    return StoreRead::Read;
}

QVector<ClipInfo> ClipsRegistry::withUnsavedChanges(QVector<ClipInfo> stored) const {
    if (m_unsaved.isEmpty()) return stored;
    QHash<QString, int> index;
    for (int i = 0; i < stored.size(); ++i)
        if (!stored.at(i).id.isEmpty()) index.insert(stored.at(i).id, i);
    QVector<ClipInfo> added;   // newest first, as m_clips holds them
    for (const ClipInfo& c : m_clips) {
        if (!m_unsaved.contains(c.id)) continue;
        const auto it = index.constFind(c.id);
        if (it != index.constEnd()) stored[*it] = c;
        else added.push_back(c);
    }
    return added + stored;
}

void ClipsRegistry::load() {
    const QString path = resolvedStorePath();
    // Reading under the lock keeps this read from overlapping another
    // instance's rewrite. Without it the read still goes ahead: it cannot
    // damage the store, and the next save re-reads under the lock anyway.
    QLockFile lock(lockPathFor(path));
    lock.tryLock(kLockWaitMs);
    QVector<ClipInfo> stored;
    QString error;
    const StoreRead read = readStore(path, &stored, &error);
    if (read == StoreRead::Unreadable || read == StoreRead::Unparseable) {
        qWarning("ClipsRegistry: could not read %s (%s); it is left as it is",
                 qPrintable(QDir::toNativeSeparators(path)), qPrintable(error));
    }
    m_clips = withUnsavedChanges(stored);
    emit changed();
}

bool ClipsRegistry::save() {
    if (!persist()) return false;
    emit changed();   // the merge may have brought in another instance's clips
    return true;
}

bool ClipsRegistry::persist() {
    const QString path = resolvedStorePath();
    auto fail = [this, &path](const QString& reason) {
        qWarning("ClipsRegistry: could not save %s: %s",
                 qPrintable(QDir::toNativeSeparators(path)), qPrintable(reason));
        emit saveFailed(reason);
        return false;
    };
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return fail(tr("its folder could not be created"));

    QLockFile lock(lockPathFor(path));
    if (!lock.tryLock(kLockWaitMs))
        return fail(tr("another MalloyStudio window is still writing it"));

    // Another instance may have written since this one last read, so what is
    // written is the store as it is now with this instance's changes on top,
    // not this instance's copy of it.
    QVector<ClipInfo> stored;
    QString error;
    switch (readStore(path, &stored, &error)) {
    case StoreRead::Read:
        break;
    case StoreRead::Missing:
        stored = m_clips;
        break;
    case StoreRead::Unreadable:
        // Whatever it holds is unknown, so writing over it could lose clips.
        return fail(tr("it could not be read (%1), so it was left as it is").arg(error));
    case StoreRead::Unparseable: {
        // Kept for whoever can repair it, and out of the way of this save.
        QString aside = path + QStringLiteral(".bad");
        for (int n = 1; QFileInfo::exists(aside); ++n)
            aside = path + QStringLiteral(".bad.%1").arg(n);
        if (!QFile::rename(path, aside))
            return fail(tr("it is damaged (%1) and could not be moved aside").arg(error));
        qWarning("ClipsRegistry: %s was damaged (%s); moved to %s",
                 qPrintable(QDir::toNativeSeparators(path)), qPrintable(error),
                 qPrintable(QDir::toNativeSeparators(aside)));
        stored = m_clips;
        break;
    }
    }

    const QVector<ClipInfo> merged = withUnsavedChanges(stored);
    QJsonArray arr;
    for (const ClipInfo& c : merged) arr.append(c.toJson());
    const QByteArray bytes = QJsonDocument(arr).toJson(QJsonDocument::Indented);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return fail(f.errorString());
    if (f.write(bytes) != bytes.size() || !f.commit()) return fail(f.errorString());

    m_clips = merged;
    m_unsaved.clear();
    return true;
}

void ClipsRegistry::addClip(const ClipInfo& clip) {
    ClipInfo c = clip;
    if (c.id.isEmpty())
        c.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_clips.prepend(c);
    m_unsaved.insert(c.id);
    persist();
    emit changed();
}

ClipInfo ClipsRegistry::registerFile(const QString& filePath, const QString& sourceProject, int durationSecs) {
    const QFileInfo fi(filePath);
    ClipInfo c;
    c.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    c.filePath = filePath;
    c.name = fi.completeBaseName().isEmpty() ? fi.fileName() : fi.completeBaseName();
    c.sourceProject = sourceProject;
    c.recordedAt = QDateTime::currentDateTime();
    c.sizeBytes = fi.exists() ? fi.size() : 0;
    c.durationSecs = durationSecs;
    addClip(c);
    return c;
}

void ClipsRegistry::setFavorite(const QString& id, bool favorite) {
    for (ClipInfo& c : m_clips) {
        if (c.id == id) {
            c.favorite = favorite;
            m_unsaved.insert(c.id);
            persist();
            emit changed();
            return;
        }
    }
}
