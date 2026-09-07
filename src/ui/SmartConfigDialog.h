#pragma once

// Shows what was detected about this machine, what settings that suggests, and
// the reason for each one. Nothing is written until Apply is pressed: a tool
// that silently rewrites the user's encoder settings is worse than no tool.

#include "platform/SmartConfig.h"

#include <QDialog>

class AudioController;
class QLabel;
class QSpinBox;
class QVBoxLayout;

class SmartConfigDialog : public QDialog {
    Q_OBJECT
public:
    // `current` is the settings in force, shown alongside each proposal so the
    // user can see exactly what would change.
    // `audio`, when given, is watched for a moment so the dialog can tell a
    // microphone that is connected and working from one that is connected and
    // silent. Passing nullptr simply leaves that unobserved.
    explicit SmartConfigDialog(const SystemProfile& profile, const OutputSettings& current,
                               AudioController* audio = nullptr, QWidget* parent = nullptr);

    // Valid after exec() returns Accepted.
    OutputSettings chosenSettings() const { return m_recommendation.output; }

private:
    void rebuild();          // re-runs the recommendation and redraws the table
    // Watches the configured input for a short window and records the loudest
    // peak seen. A single instantaneous read cannot distinguish a dead input
    // from a pause between words, so this samples over time or not at all.
    void watchMicrophone(AudioController* audio);

    SystemProfile   m_profile;
    OutputSettings  m_current;
    Recommendation  m_recommendation;

    QVBoxLayout* m_body = nullptr;   // holds the proposal rows and warnings
    QSpinBox*    m_upload = nullptr;
    QLabel*      m_micWatch = nullptr;   // "listening" line, replaced by the result
    float        m_micPeak = 0.0f;
};
