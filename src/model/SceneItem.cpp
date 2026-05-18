#include "SceneItem.h"
#include "Canvas.h"
#include "FilterEffect.h"

#include <algorithm>

namespace {
int g_nextSceneItemId = 1;
}

SceneItem::SceneItem(int sourceId, Source::Type sourceType, QObject* parent)
    : QObject(parent),
      m_id(nextId()),
      m_sourceId(sourceId),
      m_sourceType(sourceType),
      m_transform(defaultTransformFor(sourceType))
{}

void SceneItem::setVisible(bool visible) {
    if (m_visible == visible) return;
    m_visible = visible;
    emit visibilityChanged();
    emit changed();
}

void SceneItem::setLocked(bool locked) {
    if (m_locked == locked) return;
    m_locked = locked;
    emit lockChanged();
    emit changed();
}

void SceneItem::setSelected(bool selected) {
    if (m_selected == selected) return;
    m_selected = selected;
    emit selectionChanged();
    emit changed();
}

void SceneItem::setTransform(const QRectF& transform) {
    const QRectF next = MalloyCanvas::clampRect(transform);
    if (m_transform == next) return;
    m_transform = next;
    emit transformChanged();
    emit changed();
}

void SceneItem::moveBy(qreal dx, qreal dy) {
    setTransform(MalloyCanvas::snapRect(m_transform.translated(dx, dy)));
}

void SceneItem::resetTransform() {
    setTransform(defaultTransformFor(m_sourceType));
}

void SceneItem::addFilter(FilterEffect* f) {
    if (!f) return;
    f->setParent(this);
    m_filters.append(f);
    connect(f, &FilterEffect::changed, this, [this]{ emit filtersChanged(); emit changed(); });
    emit filtersChanged();
    emit changed();
}

void SceneItem::removeFilterAt(int index) {
    if (index < 0 || index >= m_filters.size()) return;
    FilterEffect* f = m_filters.takeAt(index);
    delete f;
    emit filtersChanged();
    emit changed();
}

void SceneItem::moveFilterUp(int index) {
    if (index <= 0 || index >= m_filters.size()) return;
    m_filters.swapItemsAt(index, index - 1);
    emit filtersChanged();
    emit changed();
}

void SceneItem::moveFilterDown(int index) {
    if (index < 0 || index >= m_filters.size() - 1) return;
    m_filters.swapItemsAt(index, index + 1);
    emit filtersChanged();
    emit changed();
}

QJsonArray SceneItem::filtersToJson() const {
    QJsonArray arr;
    for (const FilterEffect* f : m_filters)
        arr.append(f->toJson());
    return arr;
}

void SceneItem::filtersFromJson(const QJsonArray& arr) {
    qDeleteAll(m_filters);
    m_filters.clear();
    for (const QJsonValue& val : arr) {
        FilterEffect* f = FilterEffect::fromJson(val.toObject(), this);
        if (f) {
            connect(f, &FilterEffect::changed, this, [this]{ emit filtersChanged(); emit changed(); });
            m_filters.append(f);
        }
    }
}

SceneItem* SceneItem::duplicate(QObject* parent) const {
    auto* item = new SceneItem(m_sourceId, m_sourceType, parent);
    item->setVisible(m_visible);
    item->setLocked(false);
    item->setTransform(m_transform.translated(32.0, 32.0));
    // Deep-copy the filter chain.
    for (const FilterEffect* f : m_filters)
        item->addFilter(f->clone(item));
    return item;
}

int SceneItem::nextId() {
    return g_nextSceneItemId++;
}

void SceneItem::observeLoadedId(int id) {
    if (id >= g_nextSceneItemId) g_nextSceneItemId = id + 1;
}

void SceneItem::setIdForLoad(int id) {
    if (id <= 0) return;
    m_id = id;
    observeLoadedId(id);
}

QRectF SceneItem::defaultTransformFor(Source::Type sourceType) {
    switch (sourceType) {
        case Source::Type::DisplayCapture:
        case Source::Type::WindowCapture:
            return {0.0, 0.0, MalloyCanvas::Width, MalloyCanvas::Height};
        case Source::Type::Text:
            return {MalloyCanvas::Width * 0.25, MalloyCanvas::Height * 0.40, MalloyCanvas::Width * 0.50, 140.0};
        case Source::Type::ColorBlock:
            return {MalloyCanvas::Width * 0.30, MalloyCanvas::Height * 0.30, MalloyCanvas::Width * 0.40, MalloyCanvas::Height * 0.40};
        case Source::Type::Image:
        case Source::Type::Browser:
            return {MalloyCanvas::Width * 0.20, MalloyCanvas::Height * 0.20, MalloyCanvas::Width * 0.60, MalloyCanvas::Height * 0.60};
        case Source::Type::AudioInput:
            // Audio sources have a small non-visual footprint; user can hide/move as needed.
            return {MalloyCanvas::Width * 0.35, MalloyCanvas::Height * 0.40, MalloyCanvas::Width * 0.30, 80.0};
    }
    return {MalloyCanvas::Width * 0.25, MalloyCanvas::Height * 0.25, MalloyCanvas::Width * 0.50, MalloyCanvas::Height * 0.50};
}
