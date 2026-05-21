#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QStackedWidget;

// Settings workspace (secondary.jsx SettingsScreen): a two-pane screen with a
// group list on the left and a content area on the right. The Recording group
// is built out in full; other groups share a generic same-pattern page. This
// is the surface that will eventually consolidate the standalone dialogs.
class SettingsWorkspace : public QWidget {
    Q_OBJECT
public:
    explicit SettingsWorkspace(QWidget* parent = nullptr);

signals:
    // Emitted when the user applies Recording settings (persisted to
    // OutputSettings); MainWindow reloads and re-applies live state.
    void recordingSettingsApplied();

private:
    QWidget* buildRecordingPage();
    QWidget* buildGenericPage(const QString& title);
    void loadRecordingSettings();
    void applyRecordingSettings();
    void updateEncoderDerived();   // rate-control/CRF follow the chosen encoder

    QStackedWidget* m_stack = nullptr;
    QComboBox* m_encoderCombo = nullptr;
    QComboBox* m_rateCombo = nullptr;
    QComboBox* m_resCombo = nullptr;
    QComboBox* m_fpsCombo = nullptr;
    QComboBox* m_containerCombo = nullptr;
    QLineEdit* m_crfEdit = nullptr;
    QCheckBox* m_replayCheck = nullptr;
};
