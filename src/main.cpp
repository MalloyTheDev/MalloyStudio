#include "MainWindow.h"
#include <QApplication>
#include <QStyleFactory>
#include <QPalette>

static void applyDarkPalette(QApplication& app) {
    app.setStyle(QStyleFactory::create("Fusion"));

    QPalette p;
    p.setColor(QPalette::Window,          QColor(45, 45, 48));
    p.setColor(QPalette::WindowText,      QColor(220, 220, 220));
    p.setColor(QPalette::Base,            QColor(30, 30, 30));
    p.setColor(QPalette::AlternateBase,   QColor(45, 45, 48));
    p.setColor(QPalette::ToolTipBase,     QColor(255, 255, 255));
    p.setColor(QPalette::ToolTipText,     QColor(0, 0, 0));
    p.setColor(QPalette::Text,            QColor(220, 220, 220));
    p.setColor(QPalette::Button,          QColor(60, 60, 63));
    p.setColor(QPalette::ButtonText,      QColor(220, 220, 220));
    p.setColor(QPalette::BrightText,      QColor(255, 0, 0));
    p.setColor(QPalette::Link,            QColor(42, 130, 218));
    p.setColor(QPalette::Highlight,       QColor(42, 130, 218));
    p.setColor(QPalette::HighlightedText, QColor(0, 0, 0));
    p.setColor(QPalette::Disabled, QPalette::Text,       QColor(120, 120, 120));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(120, 120, 120));

    app.setPalette(p);
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("MalloyStudio");
    QCoreApplication::setApplicationName("MalloyStudio");

    applyDarkPalette(app);

    MainWindow w;
    w.show();
    return app.exec();
}
