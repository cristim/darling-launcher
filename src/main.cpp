// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"
#include "log.h"
#include "stagesupervisor.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QTimer>
#include <cstring>
int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], StageSupervisor::Flag) == 0) return StageSupervisor::run(argc, argv);
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
