#include "MicrophonePickerDialog.h"
#include "audio/AudioController.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

MicrophonePickerDialog::MicrophonePickerDialog(AudioController* audio, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Select Microphone"));
    setMinimumWidth(420);

    auto* layout = new QVBoxLayout(this);

    auto* prompt = new QLabel(tr("Choose a capture device to add:"), this);
    layout->addWidget(prompt);

    m_list = new QListWidget(this);
    layout->addWidget(m_list, 1);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    // Population is "live" — every time the dialog is constructed we ask the
    // AudioController to query WASAPI again. This catches USB hot-plugs the
    // user did just before opening the picker. AudioController gracefully
    // returns an empty list when COM init fails or no endpoints are active.
    m_devices = audio ? audio->enumerateInputDevices()
                      : QList<QPair<QString, QString>>{};

    if (m_devices.isEmpty()) {
        // Empty-state: no mics → show a hint and disable OK so the user can't
        // accept an empty selection. Cancel still works.
        prompt->setText(tr("No microphones detected — check Windows Sound Settings."));
        m_list->setEnabled(false);
        if (auto* ok = buttons->button(QDialogButtonBox::Ok)) ok->setEnabled(false);
    } else {
        for (const auto& d : std::as_const(m_devices)) {
            auto* item = new QListWidgetItem(d.second, m_list);
            item->setData(Qt::UserRole, d.first);
        }
        m_list->setCurrentRow(0);
        // Double-click accepts — matches MonitorPickerDialog.
        connect(m_list, &QListWidget::itemDoubleClicked,
                this,   &QDialog::accept);
    }
}

QPair<QString, QString> MicrophonePickerDialog::selectedDevice() const {
    const int row = m_list ? m_list->currentRow() : -1;
    if (row < 0 || row >= m_devices.size()) return {};
    return m_devices.at(row);
}

QPair<QString, QString> MicrophonePickerDialog::pick(AudioController* audio, QWidget* parent) {
    MicrophonePickerDialog dlg(audio, parent);
    if (dlg.exec() != QDialog::Accepted) return {};
    return dlg.selectedDevice();
}
