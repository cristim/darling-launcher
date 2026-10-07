// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"
#include <QApplication>
#include <QCommandLineParser>
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("Darling Launcher");
    QCommandLineParser parser; parser.setApplicationDescription("Host launcher for isolated Darling prefixes"); parser.addHelpOption();
    QCommandLineOption builder("prefix-builder", "Path to all-vibedarling-pr-prefix.py", "script"); parser.addOption(builder); parser.process(app);
    Window window(parser.value(builder));
    window.show();
    return app.exec();
}
