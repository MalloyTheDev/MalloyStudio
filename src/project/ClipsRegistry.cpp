#include "project/ClipsRegistry.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
    const double mb = sizeBytes / (1024.0 * 1024.0);
    if (mb >= 1024.0) return QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 1);
    if (mb >= 1.0)    return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
    return QStringLiteral("%1 KB").arg(sizeBytes / 1024.0, 0, 'f', 0);
}

ClipsRegistry::ClipsRegistry(QObject* parent) : QObject(parent) {
    load();
}

void ClipsRegistry::setStorePath(const QString& path) {
    m_storePath = path;
    load();
}

QString ClipsRegistry::resolvedStorePath() const {
    if (!m_storePath.isEmpty()) return m_storePath;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dir).filePath(QStringLiteral("clips.json"));
}

void ClipsRegistry::load() {
    m_clips.clear();
    QFile f(resolvedStorePath());
    if (!f.open(QIODevice::ReadOnly)) { emit changed(); return; }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    const QJsonArray arr = doc.isArray() ? doc.array() : doc.object().value(QStringLiteral("clips")).toArray();
    for (const auto& v : arr) m_clips.push_back(ClipInfo::fromJson(v.toObject()));
    emit changed();
}

void ClipsRegistry::save() const {
    const QString path = resolvedStorePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QJsonArray arr;
    for (const ClipInfo& c : m_clips) arr.append(c.toJson());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
    f.commit();
}

void ClipsRegistry::addClip(const ClipInfo& clip) {
    ClipInfo c = clip;
    if (c.id.isEmpty())
        c.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_clips.prepend(c);
    save();
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
        if (c.id == id) { c.favorite = favorite; save(); emit changed(); return; }
    }
}
