#include "ProjectDocument.h"
#include "model/SceneCollection.h"

#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>

bool ProjectDocument::saveToFile(const SceneCollection& scenes, const QString& filePath, QString* error) {
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = file.errorString();
        return false;
    }

    const QJsonDocument doc(scenes.toJson());
    file.write(doc.toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }

    if (error) error->clear();
    return true;
}

bool ProjectDocument::loadFromFile(SceneCollection& scenes, const QString& filePath, QString* error) {
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

    return scenes.loadFromJson(doc.object(), error);
}
