#pragma once
#include <QWidget>

class QTimer;

// Two-bar (L/R) horizontal VU meter with peak-hold tick and natural decay.
// Levels are pushed in 0..1; the widget decays on its own at ~60 Hz so the
// UI looks smooth even if the audio source is bursty.
class VuMeter : public QWidget {
    Q_OBJECT
public:
    explicit VuMeter(QWidget* parent = nullptr);

    void setLevels(float peakL, float peakR);
    void setEnabled(bool enabled);

    QSize sizeHint()        const override { return {120, 28}; }
    QSize minimumSizeHint() const override { return {60, 18}; }

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    void onDecay();
    static void decayOne(float& current, float& hold, float& holdTimer);

    float m_targetL = 0.0f, m_targetR = 0.0f; // last reported peak
    float m_curL    = 0.0f, m_curR    = 0.0f; // displayed (decayed)
    float m_holdL   = 0.0f, m_holdR   = 0.0f; // peak-hold tick
    float m_holdTL  = 0.0f, m_holdTR  = 0.0f; // hold seconds remaining
    bool  m_active  = true;
    QTimer* m_decay = nullptr;
};
