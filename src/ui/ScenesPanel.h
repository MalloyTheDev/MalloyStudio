#pragma once
#include <QWidget>

class QListWidget;
class QListWidgetItem;
class SceneCollection;

class ScenesPanel : public QWidget {
    Q_OBJECT
public:
    explicit ScenesPanel(SceneCollection* scenes, QWidget* parent = nullptr);

private slots:
    void onAddClicked();
    void onRemoveClicked();
    void onRenameClicked();
    void onItemChanged(QListWidgetItem* item);
    void onSelectionChanged();
    void onCurrentSceneChanged(int index);
    void rebuild();

private:
    SceneCollection* m_scenes;
    QListWidget*     m_list;
    bool             m_updating = false;
};
