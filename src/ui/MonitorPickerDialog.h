#pragma once
#include <QDialog>
#include "capture/MonitorInfo.h"

class QListWidget;

class MonitorPickerDialog : public QDialog {
    Q_OBJECT
public:
    explicit MonitorPickerDialog(QWidget* parent = nullptr);

    // Returns the selected MonitorInfo, or an invalid one (adapterIndex == -1) if cancelled.
    MonitorInfo selectedMonitor() const;

private:
    QListWidget*        m_list;
    QList<MonitorInfo>  m_monitors;
};
