// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"
#include <QApplication>
#include <QCommandLineParser>
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("Darling Launcher");
    QCommandLineParser parser; parser.setApplicationDescription("Host launcher for isolated Darling prefixes"); parser.addHelpOption();
    QCommandLineOption builder("prefix-builder", "Path to all-vibedarling-pr-prefix.py", "script"); parser.addOption(builder);
    QCommandLineOption mountAll("mount-all", "Mount all detected macOS volumes read-only, with desktop authentication"); parser.addOption(mountAll); parser.process(app);
    Window window(parser.value(builder), parser.isSet(mountAll));
    window.show();
    return app.exec();
}
