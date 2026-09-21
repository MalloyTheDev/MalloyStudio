#include "MainWindow.h"
#include "platform/FrameProfile.h"
#include "recording/EncoderRegistry.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QProcessEnvironment>
#include <QSettings>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("MalloyStudio");
    QCoreApplication::setApplicationName("MalloyStudio");
    // The project version in CMakeLists.txt, so a release changes it in one
    // place and nothing that shows it can drift.
    QCoreApplication::setApplicationVersion(QStringLiteral(MALLOY_VERSION));

    // Stage timing is off unless asked for. The environment variable is
    // there so a measurement script can switch it on for one run without
    // leaving it on afterwards; the setting is for a whole session at the
    // keyboard.
    const QString fromEnv =
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("MALLOY_PROFILE_FRAMES"));
    const bool profile = fromEnv.isEmpty()
        ? QSettings().value(QStringLiteral("profile/frames"), false).toBool()
        : (fromEnv != QLatin1String("0"));
    FrameProfile::setEnabled(profile);
    if (profile) qInfo("frame profiling on");

    Theme::applyTheme(app);

    // Which hardware encoders this machine can actually run. It takes a few
    // seconds and runs in the background; until it reports, only the
    // software encoders are offered or recommended.
    EncoderRegistry::startHardwareCheck(EncoderRegistry::systemProbe());

    MainWindow w;
    w.show();
    return app.exec();
}
