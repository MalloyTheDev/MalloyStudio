#include "WindowPickerDialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>     // QueryFullProcessImageNameW

namespace {

struct EnumCtx {
    quintptr        selfHwnd;
    QList<quintptr> hwnds;
    QList<QString>  titles;
    QList<QString>  entries;
};

BOOL CALLBACK enumWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<EnumCtx*>(lParam);

    // Must be visible.
    if (!IsWindowVisible(hwnd)) return TRUE;

    // Must be a top-level window (no owner).
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;

    // Skip ourselves.
    if (reinterpret_cast<quintptr>(hwnd) == ctx->selfHwnd) return TRUE;

    // Must have a non-empty title.
    wchar_t titleBuf[512] = {};
    const int titleLen = GetWindowTextW(hwnd, titleBuf, 512);
    if (titleLen <= 0) return TRUE;
    const QString title = QString::fromWCharArray(titleBuf, titleLen);

    // Try to get the process executable name.
    QString exeName;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != 0) {
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (proc) {
            wchar_t exeBuf[MAX_PATH] = {};
            DWORD exeLen = MAX_PATH;
            if (QueryFullProcessImageNameW(proc, 0, exeBuf, &exeLen)) {
                // Extract just the filename.
                const wchar_t* slash = wcsrchr(exeBuf, L'\\');
                exeName = QString::fromWCharArray(slash ? slash + 1 : exeBuf);
            }
            CloseHandle(proc);
        }
    }

    ctx->hwnds.append(reinterpret_cast<quintptr>(hwnd));
    ctx->titles.append(title);
    ctx->entries.append(exeName.isEmpty()
        ? title
        : QStringLiteral("%1  [%2]").arg(title, exeName));

    return TRUE;
}

} // namespace

WindowPickerDialog::WindowPickerDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Select Window to Capture"));
    setMinimumSize(500, 360);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("Choose which window to capture:"), this));

    m_list = new QListWidget(this);
    layout->addWidget(m_list);

    auto* btnLayout = new QHBoxLayout;
    auto* refreshBtn = new QPushButton(tr("Refresh"), this);
    connect(refreshBtn, &QPushButton::clicked, this, [this, parent] {
        const quintptr self = parent
            ? reinterpret_cast<quintptr>(parent->window()->winId())
            : 0;
        populate(self);
    });
    btnLayout->addWidget(refreshBtn);
    btnLayout->addStretch();
    layout->addLayout(btnLayout);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(m_list, &QListWidget::itemDoubleClicked, this, &QDialog::accept);

    const quintptr self = parent
        ? reinterpret_cast<quintptr>(parent->window()->winId())
        : 0;
    populate(self);
}

void WindowPickerDialog::populate(quintptr selfHwnd) {
    m_hwnds.clear();
    m_titles.clear();
    m_list->clear();

    EnumCtx ctx;
    ctx.selfHwnd = selfHwnd;
    EnumWindows(enumWindowsProc, reinterpret_cast<LPARAM>(&ctx));

    m_hwnds  = ctx.hwnds;
    m_titles = ctx.titles;
    for (const QString& entry : ctx.entries)
        m_list->addItem(entry);

    if (m_list->count() > 0)
        m_list->setCurrentRow(0);
    else
        m_list->addItem(tr("(No capturable windows found)"));
}

quintptr WindowPickerDialog::selectedHwnd() const {
    const int row = m_list->currentRow();
    return (row >= 0 && row < m_hwnds.size()) ? m_hwnds.at(row) : 0;
}

QString WindowPickerDialog::selectedTitle() const {
    const int row = m_list->currentRow();
    return (row >= 0 && row < m_titles.size()) ? m_titles.at(row) : QString();
}

std::optional<std::pair<quintptr, QString>> WindowPickerDialog::pickWindow(QWidget* parent) {
    WindowPickerDialog dlg(parent);
    if (dlg.exec() != QDialog::Accepted || dlg.selectedHwnd() == 0)
        return std::nullopt;
    return std::make_pair(dlg.selectedHwnd(), dlg.selectedTitle());
}
