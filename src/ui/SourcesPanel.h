#pragma once
#include <QWidget>

class QListWidget;
class QListWidgetItem;
class SceneCollection;

class SourcesPanel : public QWidget {
    Q_OBJECT
public:
    explicit SourcesPanel(SceneCollection* scenes, QWidget* parent = nullptr);

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

    SceneCollection* m_scenes;
    QListWidget*     m_list;
    bool             m_updating = false;
};
