#include "MonitorPickerDialog.h"
#include <QListWidget>
#include <QVBoxLayout>
#include <QDialogButtonBox>
#include <QLabel>

MonitorPickerDialog::MonitorPickerDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Select Monitor to Capture"));
    setMinimumWidth(420);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("Choose which monitor to capture:"), this));

    m_list = new QListWidget(this);
    m_monitors = enumerateMonitors();

    if (m_monitors.isEmpty()) {
        m_list->addItem(tr("(No monitors found — DXGI enumeration failed)"));
    } else {
        for (const MonitorInfo& m : m_monitors)
            m_list->addItem(m.displayName());
        m_list->setCurrentRow(0);
    }
    layout->addWidget(m_list);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(m_list, &QListWidget::itemDoubleClicked, this, &QDialog::accept);
}

MonitorInfo MonitorPickerDialog::selectedMonitor() const {
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_monitors.size()) {
        MonitorInfo invalid;
        invalid.adapterIndex = -1;
        return invalid;
    }
    return m_monitors.at(row);
}
