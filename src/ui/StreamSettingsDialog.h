#pragma once
#include "recording/StreamSettings.h"

#include <QDialog>

#include <functional>

class QComboBox;
class QLineEdit;
class QSpinBox;
class QLabel;

// Dialog for configuring RTMP streaming:
//   • Service (Twitch / YouTube / Custom)
//   • Stream key (echo-masked; stored in Windows Credential Manager)
//   • Video bitrate
//   • Keyframe interval
//
// Opened from Edit → Stream Settings.
class StreamSettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit StreamSettingsDialog(const StreamSettings& current, QWidget* parent = nullptr);

    StreamSettings settings() const { return m_settings; }

    // Accepting writes QSettings and the stream key's Credential Manager
    // entry, which a test must not touch; it can capture the write instead.
    using SaveFunction = std::function<void(const StreamSettings&)>;
    void setSaveForTesting(SaveFunction save) { m_save = std::move(save); }

private slots:
    void onServiceChanged(int index);
    void onAccepted();

private:
    StreamSettings m_settings;
    SaveFunction   m_save = [](const StreamSettings& s) { s.save(); };

    QComboBox* m_serviceCombo    = nullptr;
    QLineEdit* m_customUrlEdit   = nullptr;
    QLabel*    m_customUrlError  = nullptr;   // why OK was refused; hidden otherwise
    QLabel*    m_urlPreviewLabel = nullptr;
    QLineEdit* m_keyEdit         = nullptr;
    QSpinBox*  m_bitrateSpinBox  = nullptr;
    QSpinBox*  m_keyframeSpinBox = nullptr;
    QComboBox* m_qualityPreset   = nullptr;   // Tier 3: bundles bitrate+keyframe presets
    QLabel*    m_bitrateHint     = nullptr;   // service-recommended bitrate hint
    bool       m_applyingPreset  = false;
};
