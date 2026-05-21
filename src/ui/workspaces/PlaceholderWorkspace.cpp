#include "ui/workspaces/PlaceholderWorkspace.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"

#include <QLabel>
#include <QVBoxLayout>

PlaceholderWorkspace::PlaceholderWorkspace(const QString& iconName, const QString& title,
                                           const QString& subtitle, QWidget* parent)
    : QWidget(parent) {
    auto* v = new QVBoxLayout(this);
    v->setAlignment(Qt::AlignCenter);
    v->setSpacing(10);

    if (Icons::has(iconName)) {
        auto* icon = new QLabel(this);
        icon->setPixmap(Icons::pixmap(iconName, Theme::TextFaint, 40));
        icon->setAlignment(Qt::AlignCenter);
        v->addWidget(icon, 0, Qt::AlignHCenter);
    }

    auto* t = new QLabel(title, this);
    t->setObjectName(QStringLiteral("heroTitle"));
    t->setAlignment(Qt::AlignCenter);
    v->addWidget(t, 0, Qt::AlignHCenter);

    auto* s = new QLabel(subtitle, this);
    s->setProperty("tone", "mute");
    s->setAlignment(Qt::AlignCenter);
    s->setWordWrap(true);
    s->setMaximumWidth(420);
    v->addWidget(s, 0, Qt::AlignHCenter);

    auto* tag = Theme::makeTag(QStringLiteral("Coming soon"), QStringLiteral("accent"), this);
    v->addWidget(tag, 0, Qt::AlignHCenter);
}
