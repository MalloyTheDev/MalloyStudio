#include "Scene.h"
#include "SceneItem.h"

Scene::Scene(const QString& name, QObject* parent)
    : QObject(parent), m_name(name) {}

void Scene::setName(const QString& name) {
    if (m_name == name) return;
    m_name = name;
    emit nameChanged();
}

SceneItem* Scene::itemAt(int index) const {
    if (index < 0 || index >= m_items.size()) return nullptr;
    return m_items.at(index);
}

int Scene::indexOf(SceneItem* item) const {
    return static_cast<int>(m_items.indexOf(item));
}

int Scene::selectedIndex() const {
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i)->isSelected()) return i;
    }
    return -1;
}

SceneItem* Scene::selectedItem() const {
    return itemAt(selectedIndex());
}

void Scene::addItem(SceneItem* item) {
    if (!item) return;
    item->setParent(this);
    wireItemSignals(item);
    m_items.prepend(item);
    emit itemAdded(0);
    setSelectedIndex(0);
}

void Scene::removeItemAt(int index) {
    if (index < 0 || index >= m_items.size()) return;
    SceneItem* item = m_items.takeAt(index);
    item->deleteLater();
    emit itemRemoved(index);
    if (m_items.isEmpty()) {
        emit itemSelectionChanged(-1);
    } else {
        setSelectedIndex(qMin(index, static_cast<int>(m_items.size()) - 1));
    }
}

void Scene::moveItem(int from, int to) {
    if (from < 0 || from >= m_items.size()) return;
    if (to   < 0 || to   >= m_items.size()) return;
    if (from == to) return;
    m_items.move(from, to);
    emit itemMoved(from, to);
}

void Scene::duplicateItemAt(int index) {
    SceneItem* item = itemAt(index);
    if (!item) return;
    auto* duplicate = item->duplicate(this);
    m_items.insert(index, duplicate);
    wireItemSignals(duplicate);
    emit itemAdded(index);
    setSelectedIndex(index);
}

void Scene::setSelectedIndex(int index) {
    if (index < -1 || index >= m_items.size()) return;
    bool changed = false;
    for (int i = 0; i < m_items.size(); ++i) {
        const bool selected = i == index;
        if (m_items.at(i)->isSelected() != selected) {
            m_items.at(i)->setSelected(selected);
            changed = true;
        }
    }
    if (changed || index == -1) emit itemSelectionChanged(index);
}

void Scene::wireItemSignals(SceneItem* item) {
    connect(item, &SceneItem::changed, this, [this, item] {
        emit itemChanged(indexOf(item));
    });
    connect(item, &SceneItem::transformChanged, this, [this, item] {
        emit itemTransformChanged(indexOf(item));
    });
    connect(item, &SceneItem::visibilityChanged, this, [this, item] {
        emit itemVisibilityChanged(indexOf(item));
    });
    connect(item, &SceneItem::lockChanged, this, [this, item] {
        emit itemLockChanged(indexOf(item));
    });
}
