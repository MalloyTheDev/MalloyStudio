#include "MainWindow.h"
#include "ui/Theme.h"

#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("MalloyStudio");
    QCoreApplication::setApplicationName("MalloyStudio");

    Theme::applyTheme(app);

    MainWindow w;
    w.show();
    return app.exec();
}
