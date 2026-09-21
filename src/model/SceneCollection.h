#pragma once
#include <QObject>
#include <QSet>
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

    // Groups the edits made between the two calls into a single undo step, so a
    // layer that is created and then configured is undone as the one action the
    // user actually took. Every begin must be matched by an end; without an undo
    // stack both are no-ops.
    void beginEditGroup(const QString& text);
    void endEditGroup();
    QJsonObject snapshot() const;
    void restoreSnapshot(const QJsonObject& snapshot);
    void pushSnapshotCommand(const QString& text, const QJsonObject& before, const QJsonObject& after);
    void beginEditSession();
    void commitEditSession(const QString& text);
    void cancelEditSession();
    bool editSessionActive() const { return m_editSessionActive; }

    QJsonObject toJson() const;
    // Where a load came from, which decides the rules it gets.
    //
    // A file is someone else's content: its media paths are checked and its
    // devices are held, and both happen before anything is announced, because
    // the signals a load emits drive capture synchronously. Everything else
    // (undo, redo, a cancelled edit, a new project) is this application's own
    // state and gets neither, since applying file rules there both drops paths
    // the user chose and, for the hold, arrives too late to matter.
    enum class LoadOrigin { Internal, File };
    bool loadFromJson(const QJsonObject& root, QString* error = nullptr,
                      LoadOrigin origin = LoadOrigin::Internal);

    // Consent to open the devices a project names.
    //
    // A project file names cameras, microphones, monitors and windows, and
    // loading one used to start every device it named with no confirmation
    // anywhere. A file is not evidence of intent: ProjectRegistry seeds its
    // search with Movies and Documents and rescans at startup, so a file
    // written into either is listed in the interface and is one click from
    // opening the webcam and the microphone of whoever opens it.
    //
    // So a load from a file holds those devices until the user says yes. This
    // is deliberately not applied inside loadFromJson: undo snapshots and new
    // projects go through the same function and are this application's own
    // state rather than someone else's file. The file is the trust boundary,
    // so the hold is applied there.
    bool deviceConsentPending() const { return !m_heldDeviceSources.isEmpty(); }

    // Whether this particular source is still waiting. Held per source rather
    // than per project: allowing a project that wants a screen share should not
    // also open its camera and its microphones, and a single answer covering
    // all of them is the shape that teaches people to say yes without reading.
    bool deviceHeld(int sourceId) const { return m_heldDeviceSources.contains(sourceId); }

    // What the loaded project is asking to open, for the user to read before
    // deciding. Empty when nothing device backed was loaded.
    QStringList pendingDeviceRequests() const;

    // Called after a load from a file. Holds only if there is something to
    // hold, so a project without devices never prompts.
    void holdDeviceConsent();

    // Everything, which is what the prompt's single yes means.
    void grantDeviceConsent();

    // One source, which is what switching that source on means.
    void grantDeviceConsent(int sourceId);
    void clear();

    void addScene(const QString& name = {});

    // The current scene, creating a first one when the collection is empty.
    //
    // A source, an audio input and a camera all belong to a scene, so every
    // path that adds one needs somewhere to put it. Returning null and letting
    // the caller give up silently is what made the Add Source button do
    // nothing at all on a fresh project, which is the state the application
    // starts in.
    Scene* ensureCurrentScene();
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
    void deviceConsentChanged();
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
    // Source ids waiting for the user. Empty means nothing is held.
    QSet<int> m_heldDeviceSources;
    // Every source whose type reaches a device or the screen.
    QSet<int> deviceBackedSourceIds() const;
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
