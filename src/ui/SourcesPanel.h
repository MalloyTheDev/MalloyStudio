#pragma once
#include <QWidget>

class AudioController;
class QLabel;
class QListWidget;
class QListWidgetItem;
class SceneCollection;

class SourcesPanel : public QWidget {
    Q_OBJECT
public:
    // AudioController is required so the "Microphone" entry in the Add dialog
    // can launch a MicrophonePickerDialog. Pass the same controller MainWindow
    // uses for the AudioMixerPanel — it's already wired to enumerate WASAPI
    // capture endpoints.
    explicit SourcesPanel(SceneCollection* scenes,
                          AudioController* audio,
                          QWidget* parent = nullptr);

private slots:
    void onAddClicked();
    void onRemoveClicked();
    void onDuplicateClicked();
    void onMoveUp();
    void onMoveDown();
    void onSelectionChanged();
    void onItemDoubleClicked(QListWidgetItem* item);
    void onCurrentItemChanged(int index);
    void rebuild();

private:
    QWidget* createLayerRow(int index);

    SceneCollection* m_scenes   = nullptr;
    AudioController* m_audio    = nullptr;
    QListWidget*     m_list     = nullptr;
    QLabel*          m_emptyLabel = nullptr;   // shown when current scene has 0 layers (Tier 3)
    bool             m_updating = false;
};
