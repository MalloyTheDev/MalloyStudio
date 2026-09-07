#pragma once

// Shows what was detected about this machine, what settings that suggests, and
// the reason for each one. Nothing is written until Apply is pressed: a tool
// that silently rewrites the user's encoder settings is worse than no tool.

#include "platform/SmartConfig.h"

#include <QDialog>

class QLabel;
class QSpinBox;
class QVBoxLayout;

class SmartConfigDialog : public QDialog {
    Q_OBJECT
public:
    // `current` is the settings in force, shown alongside each proposal so the
    // user can see exactly what would change.
    explicit SmartConfigDialog(const SystemProfile& profile, const OutputSettings& current,
                               QWidget* parent = nullptr);

    // Valid after exec() returns Accepted.
    OutputSettings chosenSettings() const { return m_recommendation.output; }

private:
    void rebuild();          // re-runs the recommendation and redraws the table

    SystemProfile   m_profile;
    OutputSettings  m_current;
    Recommendation  m_recommendation;

    QVBoxLayout* m_body = nullptr;   // holds the proposal rows and warnings
    QSpinBox*    m_upload = nullptr;
};
