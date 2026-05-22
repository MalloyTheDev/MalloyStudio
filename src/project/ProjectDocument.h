#pragma once

#include <QString>

class QJsonArray;
class SceneCollection;

class ProjectDocument {
public:
    static bool saveToFile(const SceneCollection& scenes, const QString& filePath, QString* error = nullptr);
    static bool loadFromFile(SceneCollection& scenes, const QString& filePath, QString* error = nullptr);

    // v3: persist/restore the editor timeline alongside the scenes in the same
    // .malloy.json. Older files simply have no "timeline" key (timeline empty).
    static bool saveToFile(const SceneCollection& scenes, const QJsonArray& timeline,
                           const QString& filePath, QString* error = nullptr);
    static bool loadFromFile(SceneCollection& scenes, QJsonArray* timeline,
                             const QString& filePath, QString* error = nullptr);
};
