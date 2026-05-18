#pragma once
#include <QDialog>
#include <QPair>
#include <QString>

class AudioController;
class QListWidget;

// Modal dialog that enumerates active WASAPI capture endpoints and lets the
// user pick one. Mirrors MonitorPickerDialog / WindowPickerDialog so the
// "+ Add Source → Microphone" flow feels consistent with the existing
// display/window pickers.
//
// Returned pair: {deviceId GUID string, friendly name}.
// Empty deviceId means the user cancelled or no devices were available.
class MicrophonePickerDialog : public QDialog {
    Q_OBJECT
public:
    explicit MicrophonePickerDialog(AudioController* audio, QWidget* parent = nullptr);

    QPair<QString, QString> selectedDevice() const;

    // Convenience: spin the dialog, return the picked device, or {} if
    // cancelled / no devices. Matches WindowPickerDialog::pickWindow style.
    static QPair<QString, QString> pick(AudioController* audio, QWidget* parent = nullptr);

private:
    QListWidget*                       m_list = nullptr;
    QList<QPair<QString, QString>>     m_devices;
};
