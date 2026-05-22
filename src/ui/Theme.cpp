#include "ui/Theme.h"

#include <QApplication>
#include <QFile>
#include <QFont>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QStyle>
#include <QStyleFactory>
#include <QTextStream>

namespace Theme {

QString loadStyleSheet() {
    QFile f(QStringLiteral(":/styles/dark.qss"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    QTextStream in(&f);
    return in.readAll();
}

void applyTheme(QApplication& app) {
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    // Palette covers widgets/sub-controls the stylesheet doesn't reach.
    QPalette p;
    p.setColor(QPalette::Window,          BgBase);
    p.setColor(QPalette::WindowText,      Text);
    p.setColor(QPalette::Base,            Surface0);
    p.setColor(QPalette::AlternateBase,   Surface1);
    p.setColor(QPalette::ToolTipBase,     Surface0);
    p.setColor(QPalette::ToolTipText,     Text);
    p.setColor(QPalette::Text,            Text);
    p.setColor(QPalette::Button,          Surface1);
    p.setColor(QPalette::ButtonText,      Text);
    p.setColor(QPalette::BrightText,      RecHi);
    p.setColor(QPalette::Link,            AccentHi);
    p.setColor(QPalette::Highlight,       AccentDim);
    p.setColor(QPalette::HighlightedText, Text);
    p.setColor(QPalette::PlaceholderText, TextMute);
    p.setColor(QPalette::Disabled, QPalette::Text,       TextFaint);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, TextFaint);
    p.setColor(QPalette::Disabled, QPalette::WindowText, TextFaint);
    app.setPalette(p);

    QFont f(QStringLiteral("Segoe UI"));
    f.setPixelSize(13);
    app.setFont(f);

    app.setStyleSheet(loadStyleSheet());
}

void setProp(QWidget* w, const char* name, const QVariant& value) {
    if (w) w->setProperty(name, value);
}

void repolish(QWidget* w) {
    if (!w || !w->style()) return;
    w->style()->unpolish(w);
    w->style()->polish(w);
    w->update();
}

void setMono(QWidget* w, bool on) {
    setProp(w, "mono", on);
    repolish(w);
}

void setTone(QWidget* w, const QString& tone) {
    setProp(w, "tone", tone);
    repolish(w);
}

void setVariant(QPushButton* b, const QString& variant) {
    setProp(b, "variant", variant);
    repolish(b);
}

QLabel* makeTag(const QString& text, const QString& tone, QWidget* parent) {
    auto* l = new QLabel(text.toUpper(), parent);
    l->setProperty("tag", true);
    if (!tone.isEmpty()) l->setProperty("tone", tone);
    QFont f = l->font();
    f.setLetterSpacing(QFont::AbsoluteSpacing, 0.5);
    l->setFont(f);
    l->setAlignment(Qt::AlignCenter);
    return l;
}

QLabel* makeKbd(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setProperty("kbd", true);
    l->setAlignment(Qt::AlignCenter);
    return l;
}

QLabel* makeSectionHeader(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text.toUpper(), parent);
    l->setObjectName(QStringLiteral("sectionHeader"));
    QFont f = l->font();
    f.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
    l->setFont(f);
    return l;
}

QLabel* label(const QString& text, const QString& tone, int px, bool bold, bool mono, bool wrap) {
    auto* l = new QLabel(text);
    if (!tone.isEmpty()) l->setProperty("tone", tone);
    if (mono) l->setProperty("mono", true);
    QFont f = l->font();
    f.setPixelSize(px);
    if (bold) f.setWeight(QFont::DemiBold);
    l->setFont(f);
    l->setWordWrap(wrap);
    return l;
}

} // namespace Theme
