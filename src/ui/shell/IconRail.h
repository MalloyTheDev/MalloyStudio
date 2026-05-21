#pragma once

#include <QHash>
#include <QString>
#include <QWidget>

class QPushButton;

// Left workspace switcher (56px). Brand mark on top, one button per NavItem,
// help + avatar pinned to the bottom. Paints an accent bar beside the active
// button. Emits picked() when a workspace button is clicked.
class IconRail : public QWidget {
    Q_OBJECT
public:
    explicit IconRail(QWidget* parent = nullptr);

    void setActiveWorkspace(const QString& id);
    QString activeWorkspace() const { return m_active; }

signals:
    void picked(const QString& id);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void retint();

    QString m_active;
    QHash<QString, QPushButton*> m_buttons;
};
