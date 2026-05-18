#pragma once
#include "recording/OutputSettings.h"
#include <QDialog>

class QComboBox;
class QFormLayout;
class QLabel;
class QSlider;
class QSpinBox;
class QWidget;

// Modal dialog for configuring encoder / output settings.
// The video codec combo is populated from EncoderRegistry::available() so
// hardware encoders (NVENC / QSV / AMF) appear automatically when detected.
// Selecting a hardware encoder hides the CRF slider and shows the bitrate
// spinbox instead (hardware encoders use CBR, not constant quality).
class OutputSettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit OutputSettingsDialog(const OutputSettings& current, QWidget* parent = nullptr);

    OutputSettings settings() const;

private slots:
    void onCodecChanged(int index);

private:
    // Resolution
    QSpinBox*  m_width  = nullptr;
    QSpinBox*  m_height = nullptr;

    // Frame rate
    QSpinBox*  m_fps = nullptr;

    // Video codec (populated from EncoderRegistry)
    QComboBox* m_videoCodec = nullptr;

    // CRF (quality factor — software encoders only)
    QWidget*   m_crfRow   = nullptr;   // container widget for form row
    QSlider*   m_crf      = nullptr;
    QLabel*    m_crfLabel = nullptr;

    // Bitrate (CBR — hardware encoders + streaming)
    QWidget*   m_bitrateRow  = nullptr;
    QSpinBox*  m_bitrateKbps = nullptr;

    // Preset (encoding speed vs. compression)
    QComboBox* m_preset = nullptr;

    // Audio
    QSpinBox*  m_audioBitrate = nullptr;  // kbps

    // Container
    QComboBox* m_container = nullptr;

    // Replay buffer duration (0 = disabled)
    QSpinBox*  m_replayBuffer = nullptr;
};
