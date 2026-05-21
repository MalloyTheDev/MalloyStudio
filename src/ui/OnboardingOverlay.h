#pragma once

#include <QWidget>

class QFrame;
class QLabel;
class QProgressBar;
class QPushButton;
class QStackedWidget;

// First-run setup wizard (secondary.jsx Onboarding): a dim full-window overlay
// with a centered card stepping through welcome → usage → folder → devices →
// quality → density → done. Self-contained; selections are illustrative.
class OnboardingOverlay : public QWidget {
    Q_OBJECT
public:
    explicit OnboardingOverlay(QWidget* parent = nullptr);

    void openWizard();

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void goToStep(int step);

    QStackedWidget* m_steps = nullptr;
    QProgressBar* m_progress = nullptr;
    QLabel* m_stepCount = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_subtitle = nullptr;
    QPushButton* m_back = nullptr;
    QPushButton* m_next = nullptr;
    QFrame* m_card = nullptr;
    int m_step = 0;
    int m_count = 7;
};
