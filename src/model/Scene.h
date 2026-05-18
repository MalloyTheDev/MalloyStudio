#pragma once
#include <QObject>
#include <QString>
#include <QList>

class SceneItem;

class Scene : public QObject {
    Q_OBJECT
public:
    explicit Scene(const QString& name, QObject* parent = nullptr);

    QString name() const { return m_name; }
    void    setName(const QString& name);

    const QList<SceneItem*>& items() const { return m_items; }
    int        itemCount() const { return static_cast<int>(m_items.size()); }
    SceneItem* itemAt(int index) const;
    int        indexOf(SceneItem* item) const;
    int        selectedIndex() const;
    SceneItem* selectedItem() const;

    void addItem(SceneItem* item);
    void removeItemAt(int index);
    void moveItem(int from, int to);
    void duplicateItemAt(int index);
    void setSelectedIndex(int index);

signals:
    void nameChanged();
    void itemAdded(int index);
    void itemRemoved(int index);
    void itemMoved(int from, int to);
    void itemChanged(int index);
    void itemTransformChanged(int index);
    void itemVisibilityChanged(int index);
    void itemLockChanged(int index);
    void itemSelectionChanged(int index);

private:
    void wireItemSignals(SceneItem* item);

    QString        m_name;
    QList<SceneItem*> m_items;
};
