#include "ProjectDocument.h"
#include "model/SceneCollection.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

QString ProjectDocument::displayName(const QString& filePath) {
    if (filePath.isEmpty()) return QString();
    const QFileInfo fi(filePath);
    QString name = fi.fileName();
    if (name.endsWith(QStringLiteral(".malloy.json"), Qt::CaseInsensitive)) {
        name.chop(int(sizeof(".malloy.json")) - 1);
        return name;
    }
    return fi.completeBaseName();
}

bool ProjectDocument::saveToFile(const SceneCollection& scenes, const QString& filePath, QString* error) {
    return saveToFile(scenes, QJsonArray{}, filePath, error);
}

bool ProjectDocument::saveToFile(const SceneCollection& scenes, const QJsonArray& timeline,
                                 const QString& filePath, QString* error) {
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = file.errorString();
        return false;
    }

    QJsonObject obj = scenes.toJson();
    if (!timeline.isEmpty())
        obj.insert(QStringLiteral("timeline"), timeline);   // v3 editor timeline

    const QJsonDocument doc(obj);
    file.write(doc.toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }

    if (error) error->clear();
    return true;
}

bool ProjectDocument::loadFromFile(SceneCollection& scenes, const QString& filePath, QString* error) {
    return loadFromFile(scenes, nullptr, filePath, error);
}

bool ProjectDocument::loadFromFile(SceneCollection& scenes, QJsonArray* timeline,
                                   const QString& filePath, QString* error) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return false;
    }

    QJsonParseError parseError;
    // Bounded before it is read, not after. A project is a scene description
    // rather than media, so a file this large is either broken or hostile, and
    // reading it to find out is the part worth avoiding. ProjectRegistry
    // already caps its own peek at these files for the same reason.
    constexpr qint64 kMaxProjectBytes = 32 * 1024 * 1024;
    if (file.size() > kMaxProjectBytes) {
        if (error)
            *error = QStringLiteral("This project file is too large to open (%1 MB).")
                         .arg(file.size() / (1024 * 1024));
        return false;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error) *error = parseError.errorString();
        return false;
    }

    if (!doc.isObject()) {
        if (error) *error = QStringLiteral("Project root must be a JSON object.");
        return false;
    }

    const QJsonObject obj = doc.object();
    if (timeline)
        *timeline = obj.value(QStringLiteral("timeline")).toArray();   // empty for v1/v2 files
    if (!scenes.loadFromJson(obj, error)) return false;

    // Loaded from a file, so the devices it names wait for the user. Applied
    // here rather than inside loadFromJson because undo snapshots and new
    // projects use that function too and are not someone else's file.
    scenes.holdDeviceConsent();
    return true;
}
