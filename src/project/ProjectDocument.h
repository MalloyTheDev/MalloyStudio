#pragma once

#include <QString>

class SceneCollection;

class ProjectDocument {
public:
    static bool saveToFile(const SceneCollection& scenes, const QString& filePath, QString* error = nullptr);
    static bool loadFromFile(SceneCollection& scenes, const QString& filePath, QString* error = nullptr);
};
