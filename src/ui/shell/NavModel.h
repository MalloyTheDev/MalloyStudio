#pragma once

// Workspace navigation model — mirrors the NAV array in MalloyStudioJS/shell.jsx.
// Shared by IconRail (renders the buttons) and AppShell (owns the page stack).

#include <QString>
#include <QVector>

struct NavItem {
    QString id;       // stable key; matches the workspace page id
    QString label;    // human label (rail tooltip, header title)
    QString icon;     // IconFactory name
    QString kbd;      // 1..0 quick-switch key
    bool    planned = false;  // dimmed + dot (e.g. AI Lab)
};

inline const QVector<NavItem>& navItems() {
    static const QVector<NavItem> items = {
        {QStringLiteral("dashboard"), QStringLiteral("Dashboard"),    QStringLiteral("dashboard"), QStringLiteral("1"), false},
        {QStringLiteral("record"),    QStringLiteral("Recording"),    QStringLiteral("record"),    QStringLiteral("2"), false},
        {QStringLiteral("stream"),    QStringLiteral("Streaming"),    QStringLiteral("stream"),    QStringLiteral("3"), false},
        {QStringLiteral("editor"),    QStringLiteral("Editor"),       QStringLiteral("editor"),    QStringLiteral("4"), false},
        {QStringLiteral("clips"),     QStringLiteral("Clips"),        QStringLiteral("clips"),     QStringLiteral("5"), false},
        {QStringLiteral("media"),     QStringLiteral("Media"),        QStringLiteral("media"),     QStringLiteral("6"), false},
        {QStringLiteral("projects"),  QStringLiteral("Projects"),     QStringLiteral("projects"),  QStringLiteral("7"), false},
        {QStringLiteral("render"),    QStringLiteral("Render Queue"), QStringLiteral("render"),    QStringLiteral("8"), false},
        {QStringLiteral("ai"),        QStringLiteral("AI Lab"),       QStringLiteral("ai"),        QStringLiteral("9"), true},
        {QStringLiteral("settings"),  QStringLiteral("Settings"),     QStringLiteral("settings"),  QStringLiteral("0"), false},
    };
    return items;
}
