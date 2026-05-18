#pragma once
#include <QWidget>

class QLabel;
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
    SceneCollection* m_scenes      = nullptr;
    QListWidget*     m_list        = nullptr;
    QLabel*          m_emptyLabel  = nullptr;   // shown when scene count is 0 (Tier 3)
    bool             m_updating    = false;
};
