#pragma once

#include <QFrame>
#include <QString>

class QHBoxLayout;
class QLabel;
class QVBoxLayout;

// Panel chrome from tokens.css (.panel + .panel-hd + .panel-bd): a bordered
// surface with a header bar (icon + title + right-aligned tools area) over a
// content body. Used by the Recording workspace columns and Dashboard cards.
class PanelFrame : public QFrame {
    Q_OBJECT
public:
    explicit PanelFrame(const QString& title, const QString& iconName = QString(),
                        QWidget* parent = nullptr);

    // Place a single widget as the body (stretched to fill).
    void setContent(QWidget* content);
    // Or add directly to the body layout for multi-widget content.
    QVBoxLayout* bodyLayout() const { return m_body; }
    // Add a tool button/widget to the right side of the header.
    void addHeaderWidget(QWidget* w);

private:
    QHBoxLayout* m_headerRow = nullptr;
    QVBoxLayout* m_body = nullptr;
    QLabel* m_title = nullptr;
};
