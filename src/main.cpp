// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"
#include "dyldcachecli.h"
#include "libscli.h"
#include "log.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QTextStream>
#include <QTimer>
int main(int argc, char **argv) {
    if (argc > 1 && QByteArray(argv[1]) == "libs") { // no GUI: handled before QApplication exists
        QStringList args; for (int i = 2; i < argc; ++i) args << QString::fromLocal8Bit(argv[i]);
        QTextStream out(stdout), err(stderr);
        return libsMain(args, out, err);
    }
    if (argc > 1 && QByteArray(argv[1]) == "dyld-cache") {
        QStringList args; for (int i = 2; i < argc; ++i) args << QString::fromLocal8Bit(argv[i]);
        QTextStream out(stdout), err(stderr);
        return dyldCacheMain(args, out, err);
    }
    QApplication app(argc, argv);
    app.setApplicationName("Darling Launcher");
    LauncherLog::installQtHandler();
    LauncherLog::write("app", "started, data folder " + QDir::homePath() + "/.darling-launcher");
    QCommandLineParser parser; parser.setApplicationDescription("Host launcher for isolated Darling prefixes"); parser.addHelpOption();
    QCommandLineOption mountAll("mount-all", "Mount all detected macOS volumes read-only, with desktop authentication"); parser.addOption(mountAll); parser.process(app);
    Window window({}, parser.isSet(mountAll));
    window.show();
    QTimer::singleShot(0, &window, [&window] { window.offerRuntimeSetup(); });
    return app.exec();
}
