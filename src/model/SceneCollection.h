#pragma once
#include <QObject>
#include <QList>
#include <QString>
#include <QRectF>
#include <QJsonObject>
#include <QColor>

#include "Source.h"

class Scene;
class SceneItem;
class QUndoStack;

class SceneCollection : public QObject {
    Q_OBJECT
public:
    explicit SceneCollection(QObject* parent = nullptr);

    int     sceneCount()   const { return static_cast<int>(m_scenes.size()); }
    Scene*  sceneAt(int index) const;
    int     currentIndex() const { return m_currentIndex; }
    Scene*  currentScene() const;
    SceneItem* currentItem() const;
    int currentItemIndex() const;

    // Studio mode — program/preview split
    int    programIndex() const { return m_programIndex; }
    int    previewIndex() const { return m_previewIndex; }
    bool   studioMode()   const { return m_studioMode; }
    Scene* programScene() const { return sceneAt(m_programIndex); }
    void   setStudioMode(bool enabled);
    void   promotePreviewToProgram();    // stage → on-air, emits programChanged

    int sourceCount() const { return static_cast<int>(m_sources.size()); }
    Source* sourceAt(int index) const;
    Source* sourceById(int sourceId) const;
    Source* sourceForItem(const SceneItem* item) const;
    Source* sourceForCurrentItem(int index) const;
    int sourceReferenceCount(int sourceId) const;
    const QList<Source*>& sources() const { return m_sources; }

    void setUndoStack(QUndoStack* undoStack);
    QJsonObject snapshot() const;
    void restoreSnapshot(const QJsonObject& snapshot);
    void pushSnapshotCommand(const QString& text, const QJsonObject& before, const QJsonObject& after);
    void beginEditSession();
    void commitEditSession(const QString& text);
    void cancelEditSession();
    bool editSessionActive() const { return m_editSessionActive; }

    QJsonObject toJson() const;
    bool loadFromJson(const QJsonObject& root, QString* error = nullptr);
    void clear();

    void addScene(const QString& name = {});
    void removeSceneAt(int index);
    void renameSceneAt(int index, const QString& name);
    void setCurrentIndex(int index);

    Source* createSource(const QString& name, Source::Type type);
    SceneItem* addNewSourceToCurrent(const QString& name, Source::Type type,
                                     const QString& text = QString(),
                                     const QColor& color = QColor(),
                                     int adapterIndex = -1,
                                     int outputIndex = 0);
    SceneItem* addExistingSourceToCurrent(int sourceId);
    void removeCurrentItemAt(int index);
    void duplicateCurrentItemAt(int index);
    void moveCurrentItem(int from, int to);
    void selectCurrentItemAt(int index);
    void renameCurrentItemAt(int index, const QString& name);
    void setCurrentItemVisible(int index, bool visible);
    void setCurrentItemLocked(int index, bool locked);
    void setCurrentItemTransform(int index, const QRectF& transform, bool recordUndo = true);
    void beginCurrentItemTransformEdit(int index);
    void updateCurrentItemTransformEdit(int index, const QRectF& transform);
    void commitCurrentItemTransformEdit();
    void cancelCurrentItemTransformEdit();
    void resetCurrentItemTransform(int index);
    void moveCurrentItemBy(int index, qreal dx, qreal dy);
    void setCurrentSourceText(int index, const QString& text, bool recordUndo = true);
    void setCurrentSourceColor(int index, const QColor& color, bool recordUndo = true);
    void setCurrentSourceMonitor(int index, int adapterIndex, int outputIndex, bool recordUndo = true);
    void setCurrentSourceImagePath(int index, const QString& path, bool recordUndo = true);
    void setCurrentSourceWindow(int index, quintptr hwnd, const QString& title, bool recordUndo = true);
    void setCurrentSourceAudioDevice(int index, const QString& deviceId, bool recordUndo = true);
    void setCurrentSourceCamera(int index, const QString& deviceId, const QString& name, bool recordUndo = true);
    void setCurrentSourceBrowserUrl(int index, const QString& url, bool recordUndo = true);
    void setCurrentSourceBrowserRefreshHz(int index, int hz, bool recordUndo = true);

    // Overloads for WindowCapture, AudioInput, and Camera source creation.
    SceneItem* addWindowCaptureToCurrent(const QString& name, quintptr hwnd, const QString& windowTitle);
    SceneItem* addAudioInputToCurrent(const QString& name, const QString& deviceId);
    SceneItem* addCameraToCurrent(const QString& name, const QString& deviceId, const QString& deviceName);

    // Returns device IDs of visible AudioInput sources in the current scene.
    QStringList gatherVisibleAudioIds() const;

    int totalSourceCount() const;

signals:
    void sceneAdded(int index);
    void sceneRemoved(int index);
    void sceneRenamed(int index);
    void sceneAboutToChange(int fromIndex, int toIndex);
    void currentChanged(int index);
    void programChanged(int index);    // on-air scene changed (studio mode)
    void previewChanged(int index);    // staged scene changed (studio mode)
    void studioModeChanged(bool enabled);
    void itemsChanged();
    void itemAdded(int index);
    void itemRemoved(int index);
    void itemMoved(int from, int to);
    void itemChanged(int index);
    void itemTransformChanged(int index);
    void itemVisibilityChanged(int index);
    void itemLockChanged(int index);
    void itemSelectionChanged(int index);
    void sourceAdded(int sourceId);
    void sourceRemoved(int sourceId);
    void sourceChanged(int sourceId);
    void sourcesChanged();
    void collectionReset();
    // Emitted when visible AudioInput sources change (for AudioController::reconcileInputs).
    void audioInputsChanged();

private:
    void wireSceneSignals(Scene* scene);
    void wireSourceSignals(Source* source);
    Source* createSourceInternal(int id, const QString& name, Source::Type type);
    SceneItem* addItemToScene(Scene* scene, int sourceId);
    int allocateSourceId();
    void observeLoadedSourceId(int sourceId);
    void collectUnusedSources();
    void recordCommand(const QString& text, const QJsonObject& before);

    QList<Source*> m_sources;
    QList<Scene*> m_scenes;
    int           m_currentIndex = -1;
    int           m_programIndex = -1;   // on-air (TimedFrameSource renders this)
    int           m_previewIndex = -1;   // staged scene (studio mode only)
    bool          m_studioMode = false;
    int           m_nextSceneNumber = 1;
    int           m_nextSourceId = 1;
    QUndoStack*   m_undoStack = nullptr;
    bool          m_restoring = false;
    bool          m_recordUndo = true;
    bool          m_editSessionActive = false;
    QJsonObject   m_editSessionBefore;
};
