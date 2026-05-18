#pragma once

#include <QJsonArray>
#include <QList>
#include <QObject>
#include <QRectF>
#include <QString>

#include "Source.h"

class FilterEffect;

class SceneItem : public QObject {
    Q_OBJECT
public:
    explicit SceneItem(int sourceId, Source::Type sourceType, QObject* parent = nullptr);

    int id() const { return m_id; }
    int sourceId() const { return m_sourceId; }

    bool isVisible() const { return m_visible; }
    void setVisible(bool visible);

    bool isLocked() const { return m_locked; }
    void setLocked(bool locked);

    bool isSelected() const { return m_selected; }
    void setSelected(bool selected);

    QRectF transform() const { return m_transform; }
    void setTransform(const QRectF& transform);
    void moveBy(qreal dx, qreal dy);
    void resetTransform();

    // Filter chain — QObject-parented to this item.
    const QList<FilterEffect*>& filters() const { return m_filters; }
    void addFilter(FilterEffect* f);
    void removeFilterAt(int index);
    void moveFilterUp(int index);
    void moveFilterDown(int index);

    // Serialization helpers (used by SceneCollection).
    QJsonArray  filtersToJson() const;
    void        filtersFromJson(const QJsonArray& arr);

    SceneItem* duplicate(QObject* parent = nullptr) const;
    void setIdForLoad(int id);

signals:
    void changed();
    void transformChanged();
    void visibilityChanged();
    void lockChanged();
    void selectionChanged();
    void filtersChanged();

private:
    static int nextId();
    static void observeLoadedId(int id);
    static QRectF defaultTransformFor(Source::Type sourceType);

    int          m_id;
    int          m_sourceId = Source::InvalidId;
    Source::Type m_sourceType = Source::Type::ColorBlock;
    bool         m_visible = true;
    bool         m_locked = false;
    bool         m_selected = false;
    QRectF       m_transform;
    QList<FilterEffect*> m_filters;
};
