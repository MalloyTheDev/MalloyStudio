#include "ProjectDocument.h"
#include "model/SceneCollection.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

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
    return scenes.loadFromJson(obj, error);
}
