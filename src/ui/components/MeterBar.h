#pragma once

#include <QWidget>

// Horizontal audio level meter with peak hold (VuMeter from primitives.jsx):
// green→yellow→red gradient fill to `level`, a bright peak tick at `peak`.
// Values are 0..1.
class MeterBar : public QWidget {
    Q_OBJECT
public:
    explicit MeterBar(QWidget* parent = nullptr);

    void setValues(double level, double peak);

    QSize sizeHint() const override { return QSize(120, m_h); }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    double m_level = 0.0;
    double m_peak = 0.0;
    int m_h = 8;
};
