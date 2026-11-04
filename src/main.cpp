// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"
#include "log.h"
#include "cli.h"
#include "displayprobe.h"
#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QTimer>
#include <cstdio>
int main(int argc, char **argv) {
    QStringList arguments;
    for (int i = 1; i < argc; ++i) {
        const QByteArray raw(argv[i]); const QString argument = QString::fromLocal8Bit(raw);
        if (argument.toLocal8Bit() != raw) {
            QCoreApplication app(argc, argv); app.setApplicationName("Darling Launcher");
            return LauncherCli::run(app, {}, "Command arguments must round-trip through the local text encoding without loss.");
        }
        arguments << argument;
    }
    if (arguments.size() == 1 && (arguments.first() == "--internal-display-probe=xcb" || arguments.first() == "--internal-display-probe=wayland")) {
        QCoreApplication app(argc, argv); return LauncherDisplay::probe(arguments.first().section('=', 1));
    }
    const auto entry = LauncherCli::selectEntry(arguments, QProcessEnvironment::systemEnvironment(), LauncherDisplay::availableBackend);
    for (const auto &reason : entry.reasons) fprintf(stderr, "%s\n", qPrintable(reason));
    if (entry.kind != LauncherCli::EntryKind::Gui) {
        QCoreApplication app(argc, argv); app.setApplicationName("Darling Launcher");
        return LauncherCli::run(app, entry.arguments, entry.error);
    }
    LauncherCli::prepareGuiEnvironment(entry);
    QApplication app(argc, argv);
    app.setApplicationName("Darling Launcher");
    LauncherLog::installQtHandler();
    LauncherLog::write("app", "started, data folder " + QDir::homePath() + "/.darling-launcher");
    Window window({}, entry.mountAll);
    window.show();
    QTimer::singleShot(0, &window, [&window] { window.offerRuntimeSetup(); });
    return app.exec();
}
