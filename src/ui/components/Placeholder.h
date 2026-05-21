#pragma once

#include <QString>
#include <QWidget>

// Striped imagery substitute (.placeholder-fill from tokens.css): a diagonally
// hatched fill with a centered uppercase mono label, used wherever the design
// shows a thumbnail mock (dashboard cards, clips, media, projects).
// Keeps a fixed aspect ratio via heightForWidth.
class Placeholder : public QWidget {
    Q_OBJECT
public:
    explicit Placeholder(const QString& label, int ratioW = 16, int ratioH = 9,
                        QWidget* parent = nullptr);

    void setLabel(const QString& label);

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override;
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString m_label;
    int m_rw, m_rh;
};
