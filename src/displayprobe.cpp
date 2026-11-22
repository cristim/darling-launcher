// SPDX-License-Identifier: GPL-3.0-or-later
#include "displayprobe.h"
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLibrary>
#include <QProcess>
#include <cstdio>
#include <csignal>
#include <unistd.h>
struct xcb_connection_t;
struct wl_display;

namespace LauncherDisplay {
int probe(const QString &backend) {
    struct sigaction deadline{}; deadline.sa_handler = SIG_DFL; sigemptyset(&deadline.sa_mask);
    sigset_t alarms; sigemptyset(&alarms); sigaddset(&alarms, SIGALRM);
    if (sigaction(SIGALRM, &deadline, nullptr) != 0 || sigprocmask(SIG_UNBLOCK, &alarms, nullptr) != 0) { fputs("Cannot bound the private display probe.\n", stderr); return 3; }
    ::alarm(3);
    QLibrary library(backend == "xcb" ? "libxcb.so.1" : "libwayland-client.so.0");
    if (!library.load()) { fputs("Display client library is unavailable.\n", stderr); return 3; }
    if (backend == "xcb") {
        using Connect = xcb_connection_t *(*)(const char *, int *);
        using Error = int (*)(xcb_connection_t *);
        using Disconnect = void (*)(xcb_connection_t *);
        const auto connect = reinterpret_cast<Connect>(library.resolve("xcb_connect"));
        const auto error = reinterpret_cast<Error>(library.resolve("xcb_connection_has_error"));
        const auto disconnect = reinterpret_cast<Disconnect>(library.resolve("xcb_disconnect"));
        if (!connect || !error || !disconnect) { fputs("XCB client entry points are unavailable.\n", stderr); return 3; }
        auto *connection = connect(nullptr, nullptr);
        const bool ready = connection && error(connection) == 0;
        if (connection) disconnect(connection);
        if (!ready) fputs("XCB display connection or authentication failed.\n", stderr);
        return ready ? 0 : 3;
    }
    if (backend == "wayland") {
        using Connect = wl_display *(*)(const char *);
        using Disconnect = void (*)(wl_display *);
        using Roundtrip = int (*)(wl_display *);
        const auto connect = reinterpret_cast<Connect>(library.resolve("wl_display_connect"));
        const auto disconnect = reinterpret_cast<Disconnect>(library.resolve("wl_display_disconnect"));
        const auto roundtrip = reinterpret_cast<Roundtrip>(library.resolve("wl_display_roundtrip"));
        if (!connect || !disconnect || !roundtrip) { fputs("Wayland client entry points are unavailable.\n", stderr); return 3; }
        auto *connection = connect(nullptr);
        const bool ready = connection && roundtrip(connection) >= 0;
        if (connection) disconnect(connection);
        if (!ready) fputs("Wayland display round trip failed.\n", stderr);
        return ready ? 0 : 3;
    }
    return 2;
}
QString availableBackend(QStringList *reasons) {
    const QString executable = QFileInfo("/proc/self/exe").canonicalFilePath();
    if (executable.isEmpty()) { reasons->append("Cannot locate the private display probe executable."); return {}; }
    QStringList available;
    for (const auto &backend : {QString("wayland"), QString("xcb")}) {
        if (qEnvironmentVariableIsEmpty(backend == "xcb" ? "DISPLAY" : "WAYLAND_DISPLAY")) continue;
        QProcess process; auto environment = QProcessEnvironment::systemEnvironment(); environment.remove("WAYLAND_SOCKET"); process.setProcessEnvironment(environment);
        process.start(executable, {"--internal-display-probe=" + backend});
        QElapsedTimer clock; clock.start(); QString reason; QByteArray output; qint64 bytes = 0;
        do {
            process.waitForFinished(50);
            const auto errors = process.readAllStandardError(); output += errors; bytes += errors.size() + process.readAllStandardOutput().size();
            if (bytes > 4096) reason = "Display probe exceeded its output limit.";
            else if (clock.elapsed() > 1500) reason = "Display probe timed out.";
            else if (process.error() == QProcess::FailedToStart) reason = "Display probe failed to start.";
            if (!reason.isEmpty()) { process.kill(); process.waitForFinished(1000); break; }
        } while (process.state() != QProcess::NotRunning);
        if (reason.isEmpty() && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0) available << backend;
        else reasons->append(backend + ": " + (reason.isEmpty() ? (output.isEmpty() ? QString("Display probe exited unsuccessfully.") : QString::fromUtf8(output).trimmed()) : reason));
    }
    const QString preferred = qEnvironmentVariable("QT_QPA_PLATFORM").section(';', 0, 0).section(':', 0, 0);
    return available.contains(preferred) ? preferred : available.value(0);
}
}
