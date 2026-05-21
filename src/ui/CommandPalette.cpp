#include "ui/CommandPalette.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"

#include <QFrame>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QVBoxLayout>

CommandPalette::CommandPalette(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("paletteOverlay"));
    hide();

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 96, 0, 0);
    outer->setAlignment(Qt::AlignHCenter | Qt::AlignTop);

    m_card = new QFrame(this);
    m_card->setObjectName(QStringLiteral("paletteCard"));
    m_card->setFixedWidth(640);
    auto* cv = new QVBoxLayout(m_card);
    cv->setContentsMargins(0, 0, 0, 0);
    cv->setSpacing(0);

    m_search = new QLineEdit(m_card);
    m_search->setObjectName(QStringLiteral("paletteSearch"));
    m_search->setPlaceholderText(tr("Search or run a command…"));
    m_search->addAction(Icons::icon(QStringLiteral("search"), Theme::TextMute, 16), QLineEdit::LeadingPosition);
    m_search->installEventFilter(this);
    cv->addWidget(m_search);

    auto* div = new QFrame(m_card);
    div->setObjectName(QStringLiteral("divider"));
    div->setFixedHeight(1);
    cv->addWidget(div);

    m_list = new QListWidget(m_card);
    m_list->setObjectName(QStringLiteral("paletteList"));
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setIconSize(QSize(16, 16));
    m_list->setMaximumHeight(360);
    m_list->setUniformItemSizes(true);
    connect(m_list, &QListWidget::itemActivated, this, [this] { runCurrent(); });
    cv->addWidget(m_list);

    outer->addWidget(m_card);

    connect(m_search, &QLineEdit::textChanged, this, &CommandPalette::refilter);
}

void CommandPalette::setCommands(QVector<Command> commands) {
    m_commands = std::move(commands);
}

void CommandPalette::openPalette() {
    if (parentWidget()) setGeometry(parentWidget()->rect());
    m_search->clear();
    refilter(QString());
    show();
    raise();
    m_search->setFocus();
}

void CommandPalette::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (parentWidget()) setGeometry(parentWidget()->rect());
}

void CommandPalette::refilter(const QString& query) {
    m_list->clear();
    const QString q = query.trimmed().toLower();
    for (int i = 0; i < m_commands.size(); ++i) {
        const Command& c = m_commands[i];
        if (!q.isEmpty()
            && !c.title.toLower().contains(q)
            && !c.subtitle.toLower().contains(q)) {
            continue;
        }
        auto* item = new QListWidgetItem(m_list);
        item->setData(Qt::UserRole, i);
        item->setIcon(Icons::icon(c.icon, Theme::TextDim, 16));
        item->setText(c.title);
        item->setToolTip(c.subtitle);
        item->setSizeHint(QSize(0, 34));
    }
    if (m_list->count() > 0) m_list->setCurrentRow(0);
}

void CommandPalette::moveSelection(int delta) {
    const int n = m_list->count();
    if (n == 0) return;
    int row = m_list->currentRow() + delta;
    row = (row % n + n) % n;
    m_list->setCurrentRow(row);
}

void CommandPalette::runCurrent() {
    auto* item = m_list->currentItem();
    if (!item) return;
    const int idx = item->data(Qt::UserRole).toInt();
    hide();
    if (idx >= 0 && idx < m_commands.size() && m_commands[idx].run)
        m_commands[idx].run();
}

bool CommandPalette::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_search && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        switch (ke->key()) {
        case Qt::Key_Down:   moveSelection(1);  return true;
        case Qt::Key_Up:     moveSelection(-1); return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:  runCurrent();      return true;
        case Qt::Key_Escape: hide();            return true;
        default: break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void CommandPalette::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) { hide(); return; }
    QWidget::keyPressEvent(event);
}

void CommandPalette::mousePressEvent(QMouseEvent* event) {
    // Click outside the card dismisses the palette.
    if (!m_card->geometry().contains(event->pos())) hide();
    QWidget::mousePressEvent(event);
}

void CommandPalette::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0, 0, 0, 150));
}
