#include "SceneCollection.h"
#include "Canvas.h"
#include "FilterEffect.h"
#include "Scene.h"
#include "SceneItem.h"
#include "Source.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QUndoCommand>
#include <QUndoStack>
#include <utility>

namespace {
QJsonObject rectToJson(const QRectF& r) {
    return {
        {QStringLiteral("x"), r.x()},
        {QStringLiteral("y"), r.y()},
        {QStringLiteral("w"), r.width()},
        {QStringLiteral("h"), r.height()},
    };
}

QRectF rectFromJson(const QJsonObject& obj) {
    return {
        obj.value(QStringLiteral("x")).toDouble(),
        obj.value(QStringLiteral("y")).toDouble(),
        obj.value(QStringLiteral("w")).toDouble(MalloyCanvas::Width),
        obj.value(QStringLiteral("h")).toDouble(MalloyCanvas::Height),
    };
}

bool sameJson(const QJsonObject& a, const QJsonObject& b) {
    return QJsonDocument(a).toJson(QJsonDocument::Compact)
        == QJsonDocument(b).toJson(QJsonDocument::Compact);
}

QJsonObject sourceToJson(const Source* source) {
    QJsonObject sourceJson{
        {QStringLiteral("id"), source->id()},
        {QStringLiteral("name"), source->name()},
        {QStringLiteral("type"), Source::typeToId(source->type())},
        {QStringLiteral("text"), source->text()},
        {QStringLiteral("color"), source->color().name(QColor::HexArgb)},
    };

    if (source->hasMonitorConfig()) {
        sourceJson.insert(QStringLiteral("monitor"), QJsonObject{
            {QStringLiteral("configured"), true},
            {QStringLiteral("adapter"), source->adapterIndex()},
            {QStringLiteral("output"), source->outputIndex()},
        });
    }

    if (!source->imagePath().isEmpty())
        sourceJson.insert(QStringLiteral("imagePath"), source->imagePath());

    if (source->hasWindowConfig()) {
        sourceJson.insert(QStringLiteral("window"), QJsonObject{
            {QStringLiteral("hwnd"), static_cast<qint64>(source->windowHandle())},
            {QStringLiteral("title"), source->windowTitle()},
        });
    }

    if (!source->audioDeviceId().isEmpty())
        sourceJson.insert(QStringLiteral("audioDeviceId"), source->audioDeviceId());

    if (source->hasCameraConfig()) {
        sourceJson.insert(QStringLiteral("cameraDeviceId"), source->cameraDeviceId());
        sourceJson.insert(QStringLiteral("cameraName"),     source->cameraName());
    }

    if (source->hasBrowserConfig()) {
        sourceJson.insert(QStringLiteral("browserUrl"),       source->browserUrl());
        sourceJson.insert(QStringLiteral("browserRefreshHz"), source->browserRefreshHz());
    }

    return sourceJson;
}

bool validateSourceObject(const QJsonObject& sourceObject, QString* error, bool requireId) {
    if (requireId) {
        if (!sourceObject.contains(QStringLiteral("id"))
            || sourceObject.value(QStringLiteral("id")).toInt(Source::InvalidId) <= Source::InvalidId) {
            if (error) *error = QStringLiteral("Project contains a source without a valid id.");
            return false;
        }
    }

    bool typeOk = false;
    Source::typeFromId(sourceObject.value(QStringLiteral("type")).toString(), &typeOk);
    if (!typeOk) {
        if (error) *error = QStringLiteral("Project contains an unknown source type.");
        return false;
    }
    return true;
}

class SnapshotCommand final : public QUndoCommand {
public:
    SnapshotCommand(SceneCollection* collection, const QString& text,
                    QJsonObject before, QJsonObject after)
        : QUndoCommand(text),
          m_collection(collection),
          m_before(std::move(before)),
          m_after(std::move(after))
    {}

    void undo() override {
        if (m_collection) m_collection->restoreSnapshot(m_before);
    }

    void redo() override {
        if (m_skipFirstRedo) {
            m_skipFirstRedo = false;
            return;
        }
        if (m_collection) m_collection->restoreSnapshot(m_after);
    }

private:
    SceneCollection* m_collection = nullptr;
    QJsonObject m_before;
    QJsonObject m_after;
    bool m_skipFirstRedo = true;
};
}

SceneCollection::SceneCollection(QObject* parent) : QObject(parent) {}

Source* SceneCollection::sourceAt(int index) const {
    if (index < 0 || index >= m_sources.size()) return nullptr;
    return m_sources.at(index);
}

Source* SceneCollection::sourceById(int sourceId) const {
    for (Source* source : m_sources) {
        if (source->id() == sourceId) return source;
    }
    return nullptr;
}

Source* SceneCollection::sourceForItem(const SceneItem* item) const {
    return item ? sourceById(item->sourceId()) : nullptr;
}

Source* SceneCollection::sourceForCurrentItem(int index) const {
    Scene* scene = currentScene();
    return scene ? sourceForItem(scene->itemAt(index)) : nullptr;
}

int SceneCollection::sourceReferenceCount(int sourceId) const {
    int count = 0;
    for (Scene* scene : m_scenes) {
        for (int i = 0; i < scene->itemCount(); ++i) {
            if (scene->itemAt(i)->sourceId() == sourceId) ++count;
        }
    }
    return count;
}

void SceneCollection::setUndoStack(QUndoStack* undoStack) {
    m_undoStack = undoStack;
}

QJsonObject SceneCollection::snapshot() const {
    return toJson();
}

void SceneCollection::restoreSnapshot(const QJsonObject& snapshot) {
    QString ignored;
    loadFromJson(snapshot, &ignored);
}

void SceneCollection::pushSnapshotCommand(const QString& text, const QJsonObject& before, const QJsonObject& after) {
    if (!m_undoStack || m_restoring || !m_recordUndo || sameJson(before, after)) return;
    m_undoStack->push(new SnapshotCommand(this, text, before, after));
}

void SceneCollection::beginEditSession() {
    if (m_editSessionActive) return;
    m_editSessionBefore = snapshot();
    m_editSessionActive = true;
}

void SceneCollection::commitEditSession(const QString& text) {
    if (!m_editSessionActive) return;
    const QJsonObject before = m_editSessionBefore;
    m_editSessionBefore = {};
    m_editSessionActive = false;
    pushSnapshotCommand(text, before, snapshot());
}

void SceneCollection::cancelEditSession() {
    if (!m_editSessionActive) return;
    const QJsonObject before = m_editSessionBefore;
    m_editSessionBefore = {};
    m_editSessionActive = false;
    restoreSnapshot(before);
}

void SceneCollection::recordCommand(const QString& text, const QJsonObject& before) {
    pushSnapshotCommand(text, before, snapshot());
}

Scene* SceneCollection::sceneAt(int index) const {
    if (index < 0 || index >= m_scenes.size()) return nullptr;
    return m_scenes.at(index);
}

Scene* SceneCollection::currentScene() const {
    return sceneAt(m_currentIndex);
}

SceneItem* SceneCollection::currentItem() const {
    Scene* scene = currentScene();
    return scene ? scene->selectedItem() : nullptr;
}

int SceneCollection::currentItemIndex() const {
    Scene* scene = currentScene();
    return scene ? scene->selectedIndex() : -1;
}

QJsonObject SceneCollection::toJson() const {
    QJsonArray sources;
    for (const Source* source : m_sources) {
        sources.append(sourceToJson(source));
    }

    QJsonArray scenes;
    for (int si = 0; si < m_scenes.size(); ++si) {
        const Scene* scene = m_scenes.at(si);
        QJsonArray items;

        for (int ii = 0; ii < scene->itemCount(); ++ii) {
            const SceneItem* item = scene->itemAt(ii);
            QJsonObject itemObj{
                {QStringLiteral("id"), item->id()},
                {QStringLiteral("sourceId"), item->sourceId()},
                {QStringLiteral("visible"), item->isVisible()},
                {QStringLiteral("locked"), item->isLocked()},
                {QStringLiteral("transform"), rectToJson(item->transform())},
            };
            const QJsonArray filters = item->filtersToJson();
            if (!filters.isEmpty())
                itemObj.insert(QStringLiteral("filters"), filters);
            items.append(itemObj);
        }

        scenes.append(QJsonObject{
            {QStringLiteral("name"), scene->name()},
            {QStringLiteral("selectedItem"), scene->selectedIndex()},
            {QStringLiteral("items"), items},
        });
    }

    return {
        {QStringLiteral("app"), QStringLiteral("MalloyStudio")},
        {QStringLiteral("version"), 2},
        {QStringLiteral("canvas"), QJsonObject{
            {QStringLiteral("width"), MalloyCanvas::Width},
            {QStringLiteral("height"), MalloyCanvas::Height},
        }},
        {QStringLiteral("currentScene"), m_currentIndex},
        {QStringLiteral("sources"), sources},
        {QStringLiteral("scenes"), scenes},
        // v4: reserved for future per-scene audio routing. Loaders ignore
        // unknown keys, so adding fields here in v5 won't bump the version.
        {QStringLiteral("audio"), QJsonObject{}},
    };
}

bool SceneCollection::loadFromJson(const QJsonObject& root, QString* error) {
    const QString app = root.value(QStringLiteral("app")).toString(QStringLiteral("MalloyStudio"));
    if (app != QStringLiteral("MalloyStudio")) {
        if (error) *error = QStringLiteral("This is not a MalloyStudio project.");
        return false;
    }

    const int version = root.value(QStringLiteral("version")).toInt(
        root.contains(QStringLiteral("sources")) ? 2 : 1);
    if (version < 1 || version > 2) {
        if (error) *error = QStringLiteral("Unsupported MalloyStudio project version.");
        return false;
    }

    const QJsonValue scenesValue = root.value(QStringLiteral("scenes"));
    if (!scenesValue.isArray()) {
        if (error) *error = QStringLiteral("Project is missing a scenes array.");
        return false;
    }

    QSet<int> declaredSourceIds;
    if (version >= 2) {
        const QJsonValue sourcesValue = root.value(QStringLiteral("sources"));
        if (!sourcesValue.isArray()) {
            if (error) *error = QStringLiteral("Project is missing a sources array.");
            return false;
        }

        for (const QJsonValue& value : sourcesValue.toArray()) {
            const QJsonObject sourceObject = value.toObject();
            if (!validateSourceObject(sourceObject, error, true)) return false;

            const int id = sourceObject.value(QStringLiteral("id")).toInt(Source::InvalidId);
            if (declaredSourceIds.contains(id)) {
                if (error) *error = QStringLiteral("Project contains duplicate source ids.");
                return false;
            }
            declaredSourceIds.insert(id);
        }
    }

    const QJsonArray sceneArray = scenesValue.toArray();
    for (const QJsonValue& sceneValue : sceneArray) {
        const QJsonArray items = sceneValue.toObject().value(QStringLiteral("items")).toArray();
        for (const QJsonValue& itemValue : items) {
            const QJsonObject itemObject = itemValue.toObject();
            if (version >= 2) {
                const int sourceId = itemObject.value(QStringLiteral("sourceId")).toInt(Source::InvalidId);
                if (!declaredSourceIds.contains(sourceId)) {
                    if (error) *error = QStringLiteral("Project contains a layer that references a missing source.");
                    return false;
                }
            } else {
                const QJsonObject sourceObject = itemObject.value(QStringLiteral("source")).toObject();
                if (sourceObject.isEmpty() || !validateSourceObject(sourceObject, error, false)) return false;
            }
        }
    }

    const bool oldRecordUndo = m_recordUndo;
    m_recordUndo = false;
    m_restoring = true;

    qDeleteAll(m_scenes);
    m_scenes.clear();
    qDeleteAll(m_sources);
    m_sources.clear();
    m_currentIndex = -1;
    m_nextSceneNumber = 1;
    m_nextSourceId = 1;

    if (version >= 2) {
        for (const QJsonValue& sourceValue : root.value(QStringLiteral("sources")).toArray()) {
            const QJsonObject sourceObject = sourceValue.toObject();
            bool typeOk = false;
            const Source::Type type = Source::typeFromId(sourceObject.value(QStringLiteral("type")).toString(), &typeOk);
            Source* source = createSourceInternal(sourceObject.value(QStringLiteral("id")).toInt(), 
                                                  sourceObject.value(QStringLiteral("name")).toString(Source::typeToString(type)),
                                                  type);
            source->setText(sourceObject.value(QStringLiteral("text")).toString(source->text()));
            source->setColor(QColor(sourceObject.value(QStringLiteral("color")).toString(source->color().name(QColor::HexArgb))));

            const QJsonObject monitor = sourceObject.value(QStringLiteral("monitor")).toObject();
            if (monitor.value(QStringLiteral("configured")).toBool(false)) {
                source->setMonitor(monitor.value(QStringLiteral("adapter")).toInt(-1),
                                   monitor.value(QStringLiteral("output")).toInt(0));
            }

            const QString imagePath = sourceObject.value(QStringLiteral("imagePath")).toString();
            if (!imagePath.isEmpty()) source->setImagePath(imagePath);

            const QJsonObject windowObj = sourceObject.value(QStringLiteral("window")).toObject();
            if (!windowObj.isEmpty()) {
                source->setWindow(static_cast<quintptr>(windowObj.value(QStringLiteral("hwnd")).toInteger()),
                                  windowObj.value(QStringLiteral("title")).toString());
            }

            const QString audioDeviceId = sourceObject.value(QStringLiteral("audioDeviceId")).toString();
            if (!audioDeviceId.isEmpty()) source->setAudioDeviceId(audioDeviceId);

            const QString cameraDeviceId = sourceObject.value(QStringLiteral("cameraDeviceId")).toString();
            if (!cameraDeviceId.isEmpty())
                source->setCamera(cameraDeviceId, sourceObject.value(QStringLiteral("cameraName")).toString());

            const QString browserUrl = sourceObject.value(QStringLiteral("browserUrl")).toString();
            if (!browserUrl.isEmpty()) {
                source->setBrowserUrl(browserUrl);
                source->setBrowserRefreshHz(sourceObject.value(QStringLiteral("browserRefreshHz")).toInt(10));
            }
        }
    }

    for (const QJsonValue& sceneValue : sceneArray) {
        const QJsonObject sceneObject = sceneValue.toObject();
        auto* scene = new Scene(sceneObject.value(QStringLiteral("name")).toString(QStringLiteral("Scene")), this);
        wireSceneSignals(scene);
        m_scenes.append(scene);

        const QJsonArray items = sceneObject.value(QStringLiteral("items")).toArray();
        for (int i = items.size() - 1; i >= 0; --i) {
            const QJsonObject itemObject = items.at(i).toObject();
            int sourceId = itemObject.value(QStringLiteral("sourceId")).toInt(Source::InvalidId);

            if (version == 1) {
                const QJsonObject sourceObject = itemObject.value(QStringLiteral("source")).toObject();
                bool typeOk = false;
                const Source::Type type = Source::typeFromId(sourceObject.value(QStringLiteral("type")).toString(), &typeOk);
                Source* source = createSourceInternal(allocateSourceId(),
                                                      sourceObject.value(QStringLiteral("name")).toString(Source::typeToString(type)),
                                                      type);
                source->setText(sourceObject.value(QStringLiteral("text")).toString(source->text()));
                source->setColor(QColor(sourceObject.value(QStringLiteral("color")).toString(source->color().name(QColor::HexArgb))));

                const QJsonObject monitor = sourceObject.value(QStringLiteral("monitor")).toObject();
                if (monitor.value(QStringLiteral("configured")).toBool(false)) {
                    source->setMonitor(monitor.value(QStringLiteral("adapter")).toInt(-1),
                                       monitor.value(QStringLiteral("output")).toInt(0));
                }
                sourceId = source->id();
            }

            Source* source = sourceById(sourceId);
            auto* item = new SceneItem(sourceId, source ? source->type() : Source::Type::ColorBlock);
            item->setIdForLoad(itemObject.value(QStringLiteral("id")).toInt(item->id()));
            item->setVisible(itemObject.value(QStringLiteral("visible")).toBool(true));
            item->setLocked(itemObject.value(QStringLiteral("locked")).toBool(false));
            item->setTransform(rectFromJson(itemObject.value(QStringLiteral("transform")).toObject()));
            item->filtersFromJson(itemObject.value(QStringLiteral("filters")).toArray());
            scene->addItem(item);
        }

        scene->setSelectedIndex(sceneObject.value(QStringLiteral("selectedItem")).toInt(-1));
    }

    const int requestedCurrent = root.value(QStringLiteral("currentScene")).toInt(m_scenes.isEmpty() ? -1 : 0);
    if (!m_scenes.isEmpty())
        m_currentIndex = qBound(0, requestedCurrent, static_cast<int>(m_scenes.size()) - 1);
    // Program and preview start on the same scene; studio mode defaults off.
    m_programIndex = m_currentIndex;
    m_previewIndex = m_currentIndex;
    m_studioMode   = false;

    m_nextSceneNumber = static_cast<int>(m_scenes.size()) + 1;
    collectUnusedSources();

    m_restoring = false;
    m_recordUndo = oldRecordUndo;

    emit collectionReset();
    emit currentChanged(m_currentIndex);
    emit sourcesChanged();
    emit itemsChanged();
    emit itemSelectionChanged(currentItemIndex());

    if (error) error->clear();
    return true;
}

void SceneCollection::clear() {
    m_studioMode = false;
    QJsonObject empty{
        {QStringLiteral("app"), QStringLiteral("MalloyStudio")},
        {QStringLiteral("version"), 2},
        {QStringLiteral("currentScene"), -1},
        {QStringLiteral("sources"), QJsonArray{}},
        {QStringLiteral("scenes"), QJsonArray{}},
    };
    QString ignored;
    loadFromJson(empty, &ignored);
}

Source* SceneCollection::createSource(const QString& name, Source::Type type) {
    const QJsonObject before = snapshot();
    Source* source = createSourceInternal(allocateSourceId(),
                                          name.trimmed().isEmpty() ? Source::typeToString(type) : name.trimmed(),
                                          type);
    recordCommand(QStringLiteral("Create Source"), before);
    return source;
}

void SceneCollection::addScene(const QString& name) {
    const QJsonObject before = snapshot();
    const QString actualName = name.isEmpty()
        ? QStringLiteral("Scene %1").arg(m_nextSceneNumber++)
        : name;
    auto* scene = new Scene(actualName, this);
    wireSceneSignals(scene);
    m_scenes.append(scene);
    const int newIndex = static_cast<int>(m_scenes.size()) - 1;
    emit sceneAdded(newIndex);
    setCurrentIndex(newIndex);
    recordCommand(QStringLiteral("Add Scene"), before);
}

void SceneCollection::removeSceneAt(int index) {
    if (index < 0 || index >= m_scenes.size()) return;
    const QJsonObject before = snapshot();
    Scene* s = m_scenes.takeAt(index);
    delete s;
    collectUnusedSources();
    emit sceneRemoved(index);

    if (m_scenes.isEmpty()) {
        m_currentIndex = -1;
        emit currentChanged(-1);
        emit itemsChanged();
        emit itemSelectionChanged(-1);
    } else {
        const int newIndex = qMin(index, static_cast<int>(m_scenes.size()) - 1);
        m_currentIndex = newIndex;
        emit currentChanged(newIndex);
        emit itemsChanged();
        emit itemSelectionChanged(currentItemIndex());
    }
    recordCommand(QStringLiteral("Remove Scene"), before);
}

void SceneCollection::renameSceneAt(int index, const QString& name) {
    if (index < 0 || index >= m_scenes.size()) return;
    const QString next = name.trimmed();
    if (next.isEmpty() || m_scenes[index]->name() == next) return;
    const QJsonObject before = snapshot();
    m_scenes[index]->setName(next);
    emit sceneRenamed(index);
    recordCommand(QStringLiteral("Rename Scene"), before);
}

void SceneCollection::setCurrentIndex(int index) {
    if (index == m_currentIndex) return;
    if (index < -1 || index >= m_scenes.size()) return;
    if (!m_restoring) emit sceneAboutToChange(m_currentIndex, index);
    m_currentIndex = index;
    emit currentChanged(index);
    emit itemsChanged();
    emit itemSelectionChanged(currentItemIndex());
    emit audioInputsChanged();

    // In studio mode, setCurrentIndex stages the scene (sets preview).
    // In live mode, it also puts it on-air (sets program).
    if (m_studioMode) {
        if (m_previewIndex != index) {
            m_previewIndex = index;
            emit previewChanged(index);
        }
    } else {
        if (m_programIndex != index) {
            m_programIndex = index;
            emit programChanged(index);
        }
    }
}

void SceneCollection::setStudioMode(bool enabled) {
    if (m_studioMode == enabled) return;
    m_studioMode = enabled;
    if (!enabled) {
        // Collapsing back to live: make current scene the program.
        if (m_programIndex != m_currentIndex) {
            m_programIndex = m_currentIndex;
            emit programChanged(m_programIndex);
        }
    } else {
        // Entering studio mode: preview starts on the same scene as program.
        m_previewIndex = m_programIndex;
        emit previewChanged(m_previewIndex);
    }
    emit studioModeChanged(enabled);
}

void SceneCollection::promotePreviewToProgram() {
    if (m_programIndex == m_previewIndex) return;
    m_programIndex = m_previewIndex;
    emit programChanged(m_programIndex);
    // Keep audio consistent with what's now on-air.
    emit audioInputsChanged();
}

SceneItem* SceneCollection::addNewSourceToCurrent(const QString& name, Source::Type type,
                                                  const QString& text,
                                                  const QColor& color,
                                                  int adapterIndex,
                                                  int outputIndex) {
    Scene* scene = currentScene();
    if (!scene) return nullptr;

    const QJsonObject before = snapshot();
    Source* source = createSourceInternal(allocateSourceId(),
                                          name.trimmed().isEmpty() ? Source::typeToString(type) : name.trimmed(),
                                          type);
    if (!text.isNull()) source->setText(text);
    if (color.isValid()) source->setColor(color);
    if (adapterIndex >= 0) source->setMonitor(adapterIndex, outputIndex);

    SceneItem* item = addItemToScene(scene, source->id());
    recordCommand(QStringLiteral("Add Layer"), before);
    return item;
}

SceneItem* SceneCollection::addExistingSourceToCurrent(int sourceId) {
    Scene* scene = currentScene();
    if (!scene || !sourceById(sourceId)) return nullptr;
    const QJsonObject before = snapshot();
    SceneItem* item = addItemToScene(scene, sourceId);
    recordCommand(QStringLiteral("Add Layer"), before);
    return item;
}

void SceneCollection::removeCurrentItemAt(int index) {
    Scene* scene = currentScene();
    if (!scene || !scene->itemAt(index)) return;
    const QJsonObject before = snapshot();
    scene->removeItemAt(index);
    collectUnusedSources();
    recordCommand(QStringLiteral("Remove Layer"), before);
}

void SceneCollection::duplicateCurrentItemAt(int index) {
    Scene* scene = currentScene();
    if (!scene || !scene->itemAt(index)) return;
    const QJsonObject before = snapshot();
    scene->duplicateItemAt(index);
    recordCommand(QStringLiteral("Duplicate Layer"), before);
}

void SceneCollection::moveCurrentItem(int from, int to) {
    Scene* scene = currentScene();
    if (!scene || !scene->itemAt(from) || to < 0 || to >= scene->itemCount() || from == to) return;
    const QJsonObject before = snapshot();
    scene->moveItem(from, to);
    scene->setSelectedIndex(to);
    recordCommand(QStringLiteral("Reorder Layer"), before);
}

void SceneCollection::selectCurrentItemAt(int index) {
    if (Scene* scene = currentScene()) scene->setSelectedIndex(index);
}

void SceneCollection::renameCurrentItemAt(int index, const QString& name) {
    Source* source = sourceForCurrentItem(index);
    const QString next = name.trimmed();
    if (!source || next.isEmpty() || source->name() == next) return;
    const QJsonObject before = snapshot();
    source->setName(next);
    recordCommand(QStringLiteral("Rename Source"), before);
}

void SceneCollection::setCurrentItemVisible(int index, bool visible) {
    SceneItem* item = currentScene() ? currentScene()->itemAt(index) : nullptr;
    if (!item || item->isVisible() == visible) return;
    const QJsonObject before = snapshot();
    item->setVisible(visible);
    recordCommand(QStringLiteral("Toggle Layer Visibility"), before);
}

void SceneCollection::setCurrentItemLocked(int index, bool locked) {
    SceneItem* item = currentScene() ? currentScene()->itemAt(index) : nullptr;
    if (!item || item->isLocked() == locked) return;
    const QJsonObject before = snapshot();
    item->setLocked(locked);
    recordCommand(QStringLiteral("Toggle Layer Lock"), before);
}

void SceneCollection::setCurrentItemTransform(int index, const QRectF& transform, bool recordUndo) {
    SceneItem* item = currentScene() ? currentScene()->itemAt(index) : nullptr;
    if (!item || item->transform() == MalloyCanvas::clampRect(transform)) return;
    QJsonObject before;
    if (recordUndo) before = snapshot();
    item->setTransform(transform);
    if (recordUndo) recordCommand(QStringLiteral("Transform Layer"), before);
}

void SceneCollection::beginCurrentItemTransformEdit(int index) {
    if (!currentScene() || !currentScene()->itemAt(index)) return;
    beginEditSession();
}

void SceneCollection::updateCurrentItemTransformEdit(int index, const QRectF& transform) {
    setCurrentItemTransform(index, transform, false);
}

void SceneCollection::commitCurrentItemTransformEdit() {
    commitEditSession(QStringLiteral("Transform Layer"));
}

void SceneCollection::cancelCurrentItemTransformEdit() {
    cancelEditSession();
}

void SceneCollection::resetCurrentItemTransform(int index) {
    SceneItem* item = currentScene() ? currentScene()->itemAt(index) : nullptr;
    if (!item) return;
    const QJsonObject before = snapshot();
    item->resetTransform();
    recordCommand(QStringLiteral("Reset Layer Transform"), before);
}

void SceneCollection::moveCurrentItemBy(int index, qreal dx, qreal dy) {
    SceneItem* item = currentScene() ? currentScene()->itemAt(index) : nullptr;
    if (!item || item->isLocked()) return;
    const QJsonObject before = snapshot();
    item->moveBy(dx, dy);
    recordCommand(QStringLiteral("Nudge Layer"), before);
}

void SceneCollection::setCurrentSourceText(int index, const QString& text, bool recordUndo) {
    Source* source = sourceForCurrentItem(index);
    if (!source || source->text() == text) return;
    QJsonObject before;
    if (recordUndo) before = snapshot();
    source->setText(text);
    if (recordUndo) recordCommand(QStringLiteral("Edit Text"), before);
}

void SceneCollection::setCurrentSourceColor(int index, const QColor& color, bool recordUndo) {
    Source* source = sourceForCurrentItem(index);
    if (!source || !color.isValid() || source->color() == color) return;
    QJsonObject before;
    if (recordUndo) before = snapshot();
    source->setColor(color);
    if (recordUndo) recordCommand(QStringLiteral("Change Color"), before);
}

void SceneCollection::setCurrentSourceMonitor(int index, int adapterIndex, int outputIndex, bool recordUndo) {
    Source* source = sourceForCurrentItem(index);
    if (!source || (source->adapterIndex() == adapterIndex && source->outputIndex() == outputIndex)) return;
    QJsonObject before;
    if (recordUndo) before = snapshot();
    source->setMonitor(adapterIndex, outputIndex);
    if (recordUndo) recordCommand(QStringLiteral("Change Monitor"), before);
}

void SceneCollection::setCurrentSourceImagePath(int index, const QString& path, bool recordUndo) {
    Source* source = sourceForCurrentItem(index);
    if (!source || source->imagePath() == path) return;
    QJsonObject before;
    if (recordUndo) before = snapshot();
    source->setImagePath(path);
    if (recordUndo) recordCommand(QStringLiteral("Change Image"), before);
}

void SceneCollection::setCurrentSourceWindow(int index, quintptr hwnd, const QString& title, bool recordUndo) {
    Source* source = sourceForCurrentItem(index);
    if (!source || (source->windowHandle() == hwnd && source->windowTitle() == title)) return;
    QJsonObject before;
    if (recordUndo) before = snapshot();
    source->setWindow(hwnd, title);
    if (recordUndo) recordCommand(QStringLiteral("Change Window"), before);
}

void SceneCollection::setCurrentSourceAudioDevice(int index, const QString& deviceId, bool recordUndo) {
    Source* source = sourceForCurrentItem(index);
    if (!source || source->audioDeviceId() == deviceId) return;
    QJsonObject before;
    if (recordUndo) before = snapshot();
    source->setAudioDeviceId(deviceId);
    if (recordUndo) recordCommand(QStringLiteral("Change Audio Device"), before);
    emit audioInputsChanged();
}

void SceneCollection::setCurrentSourceCamera(int index, const QString& deviceId,
                                             const QString& name, bool recordUndo) {
    Source* source = sourceForCurrentItem(index);
    if (!source || source->cameraDeviceId() == deviceId) return;
    QJsonObject before;
    if (recordUndo) before = snapshot();
    source->setCamera(deviceId, name);
    if (recordUndo) recordCommand(QStringLiteral("Change Camera Device"), before);
}

void SceneCollection::setCurrentSourceBrowserUrl(int index, const QString& url, bool recordUndo) {
    Source* source = sourceForCurrentItem(index);
    if (!source || source->browserUrl() == url) return;
    QJsonObject before;
    if (recordUndo) before = snapshot();
    source->setBrowserUrl(url);
    if (recordUndo) recordCommand(QStringLiteral("Change Browser URL"), before);
}

void SceneCollection::setCurrentSourceBrowserRefreshHz(int index, int hz, bool recordUndo) {
    Source* source = sourceForCurrentItem(index);
    if (!source || source->browserRefreshHz() == hz) return;
    QJsonObject before;
    if (recordUndo) before = snapshot();
    source->setBrowserRefreshHz(hz);
    if (recordUndo) recordCommand(QStringLiteral("Change Browser Refresh Rate"), before);
}

SceneItem* SceneCollection::addWindowCaptureToCurrent(const QString& name, quintptr hwnd, const QString& windowTitle) {
    Scene* scene = currentScene();
    if (!scene) return nullptr;
    const QJsonObject before = snapshot();
    const QString displayName = name.trimmed().isEmpty() ? Source::typeToString(Source::Type::WindowCapture) : name.trimmed();
    Source* source = createSourceInternal(allocateSourceId(), displayName, Source::Type::WindowCapture);
    source->setWindow(hwnd, windowTitle);
    SceneItem* item = addItemToScene(scene, source->id());
    recordCommand(QStringLiteral("Add Window Capture"), before);
    return item;
}

SceneItem* SceneCollection::addCameraToCurrent(const QString& name, const QString& deviceId,
                                               const QString& deviceName) {
    Scene* scene = currentScene();
    if (!scene) return nullptr;
    const QJsonObject before = snapshot();
    const QString displayName = name.trimmed().isEmpty()
        ? (deviceName.isEmpty() ? Source::typeToString(Source::Type::Camera) : deviceName)
        : name.trimmed();
    Source* source = createSourceInternal(allocateSourceId(), displayName, Source::Type::Camera);
    source->setCamera(deviceId, deviceName);
    SceneItem* item = addItemToScene(scene, source->id());
    recordCommand(QStringLiteral("Add Camera"), before);
    return item;
}

SceneItem* SceneCollection::addAudioInputToCurrent(const QString& name, const QString& deviceId) {
    Scene* scene = currentScene();
    if (!scene) return nullptr;
    const QJsonObject before = snapshot();
    const QString displayName = name.trimmed().isEmpty() ? Source::typeToString(Source::Type::AudioInput) : name.trimmed();
    Source* source = createSourceInternal(allocateSourceId(), displayName, Source::Type::AudioInput);
    source->setAudioDeviceId(deviceId);
    SceneItem* item = addItemToScene(scene, source->id());
    recordCommand(QStringLiteral("Add Audio Input"), before);
    emit audioInputsChanged();
    return item;
}

QStringList SceneCollection::gatherVisibleAudioIds() const {
    QStringList ids;
    // Use the on-air (program) scene, not the staged/current scene.
    if (const Scene* s = sceneAt(m_programIndex)) {
        for (int i = 0; i < s->itemCount(); ++i) {
            const SceneItem* item = s->itemAt(i);
            if (!item->isVisible()) continue;
            const Source* src = sourceById(item->sourceId());
            if (src && src->type() == Source::Type::AudioInput && !src->audioDeviceId().isEmpty())
                ids << src->audioDeviceId();
        }
    }
    return ids;
}

int SceneCollection::totalSourceCount() const {
    int total = 0;
    for (auto* s : m_scenes) total += s->itemCount();
    return total;
}

void SceneCollection::wireSceneSignals(Scene* scene) {
    connect(scene, &Scene::itemAdded, this, [this, scene](int index) {
        if (m_restoring || currentScene() != scene) return;
        emit itemsChanged();
        emit itemAdded(index);
    });
    connect(scene, &Scene::itemRemoved, this, [this, scene](int index) {
        if (m_restoring || currentScene() != scene) return;
        emit itemsChanged();
        emit itemRemoved(index);
    });
    connect(scene, &Scene::itemMoved, this, [this, scene](int from, int to) {
        if (m_restoring || currentScene() != scene) return;
        emit itemsChanged();
        emit itemMoved(from, to);
    });
    connect(scene, &Scene::itemChanged, this, [this, scene](int index) {
        if (m_restoring || currentScene() != scene) return;
        emit itemsChanged();
        emit itemChanged(index);
    });
    connect(scene, &Scene::itemTransformChanged, this, [this, scene](int index) {
        if (m_restoring || currentScene() != scene) return;
        emit itemsChanged();
        emit itemTransformChanged(index);
    });
    connect(scene, &Scene::itemVisibilityChanged, this, [this, scene](int index) {
        if (m_restoring || currentScene() != scene) return;
        emit itemsChanged();
        emit itemVisibilityChanged(index);
        emit audioInputsChanged();
    });
    connect(scene, &Scene::itemLockChanged, this, [this, scene](int index) {
        if (m_restoring || currentScene() != scene) return;
        emit itemsChanged();
        emit itemLockChanged(index);
    });
    connect(scene, &Scene::itemSelectionChanged, this, [this, scene](int index) {
        if (!m_restoring && currentScene() == scene) emit itemSelectionChanged(index);
    });
}

void SceneCollection::wireSourceSignals(Source* source) {
    connect(source, &Source::nameChanged, this, [this, source] {
        if (m_restoring) return;
        emit sourceChanged(source->id());
        emit sourcesChanged();
        emit itemsChanged();
    });
    connect(source, &Source::settingsChanged, this, [this, source] {
        if (m_restoring) return;
        emit sourceChanged(source->id());
        emit itemsChanged();
    });
    connect(source, &Source::monitorChanged, this, [this, source] {
        if (m_restoring) return;
        emit sourceChanged(source->id());
        emit itemsChanged();
    });
}

Source* SceneCollection::createSourceInternal(int id, const QString& name, Source::Type type) {
    observeLoadedSourceId(id);
    auto* source = new Source(id, name, type, this);
    wireSourceSignals(source);
    m_sources.append(source);
    if (!m_restoring) {
        emit sourceAdded(id);
        emit sourcesChanged();
    }
    return source;
}

SceneItem* SceneCollection::addItemToScene(Scene* scene, int sourceId) {
    Source* source = sourceById(sourceId);
    if (!scene || !source) return nullptr;
    auto* item = new SceneItem(sourceId, source->type());
    scene->addItem(item);
    return item;
}

int SceneCollection::allocateSourceId() {
    return m_nextSourceId++;
}

void SceneCollection::observeLoadedSourceId(int sourceId) {
    if (sourceId >= m_nextSourceId) m_nextSourceId = sourceId + 1;
}

void SceneCollection::collectUnusedSources() {
    QSet<int> used;
    for (Scene* scene : m_scenes) {
        for (int i = 0; i < scene->itemCount(); ++i) {
            used.insert(scene->itemAt(i)->sourceId());
        }
    }

    bool changed = false;
    for (int i = m_sources.size() - 1; i >= 0; --i) {
        Source* source = m_sources.at(i);
        if (used.contains(source->id())) continue;
        const int sourceId = source->id();
        m_sources.removeAt(i);
        delete source;
        changed = true;
        if (!m_restoring) emit sourceRemoved(sourceId);
    }

    if (changed && !m_restoring) emit sourcesChanged();
}
