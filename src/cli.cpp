// SPDX-License-Identifier: GPL-3.0-or-later
#include "cli.h"
#include "core.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSocketNotifier>
#include <QTimer>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

namespace LauncherCli {
Entry selectEntry(QStringList arguments, const QProcessEnvironment &environment, const std::function<QString(QStringList *)> &probe) {
    Entry entry;
    const QString forced = environment.value("DARLING_LAUNCHER_HEADLESS");
    if (environment.contains("DARLING_LAUNCHER_HEADLESS") && forced != "0" && forced != "1") { entry.kind = EntryKind::Error; entry.error = "DARLING_LAUNCHER_HEADLESS must be 0 or 1 when set."; return entry; }
    const bool headless = arguments.removeAll("--headless") > 0 || forced == "1";
    entry.arguments = arguments;
    if (headless || (!arguments.isEmpty() && arguments != QStringList{"--mount-all"})) return entry;
    entry.backend = probe(&entry.reasons);
    if (!entry.backend.isEmpty()) { entry.kind = EntryKind::Gui; entry.mountAll = !arguments.isEmpty(); }
    return entry;
}
void prepareGuiEnvironment(const Entry &entry) {
    if (entry.kind != EntryKind::Gui) return;
    if (entry.backend == "wayland") qunsetenv("WAYLAND_SOCKET");
    qputenv("QT_QPA_PLATFORM", entry.backend.toUtf8());
}
QString helpText() {
    return "Usage: darling-launcher [--headless] [help | --help]\n"
           "       darling-launcher [--headless] apps list --volume PATH\n"
           "       darling-launcher [--mount-all]  (requires a reachable desktop)\n\n"
           "help      Show this help without loading a Qt display plugin or writing settings.\n"
           "apps list List discovered .app bundles from the explicitly named source directory.\n"
           "--headless or DARLING_LAUNCHER_HEADLESS=1 forces CLI mode on a desktop.\n"
           "The environment variable accepts 0 or 1; 0 keeps automatic display detection.\n"
           "--mount-all is GUI-only; headless invocation never mounts implicitly.\n";
}
static QJsonObject failure(const QString &message) {
    return {{"ok", false}, {"exitCode", 2}, {"error", QJsonObject{{"code", "invalid-input"}, {"message", message}}}};
}
static QJsonObject operationFailure(const QString &message) {
    return {{"ok", false}, {"exitCode", 3}, {"error", QJsonObject{{"code", "source-unavailable"}, {"message", message}}}};
}
static bool helpRequest(const QStringList &arguments) {
    return arguments.isEmpty() || arguments == QStringList{"help"} || arguments == QStringList{"--help"} || arguments == QStringList{"-h"};
}
QJsonObject dispatch(const QStringList &arguments) {
    if (helpRequest(arguments)) return {{"ok", true}, {"exitCode", 0}, {"data", QJsonObject{{"help", helpText()}}}};
    if (arguments.size() >= 2 && arguments[0] == "apps" && arguments[1] == "list") {
        if (arguments.size() != 4 || arguments[2] != "--volume" || arguments[3].isEmpty())
            return failure("Usage: darling-launcher apps list --volume PATH");
        QStringList bundles; QString error;
        const auto status = LauncherCore::listAppsChecked(arguments[3], &bundles, &error);
        if (status == LauncherCore::AppDiscoveryStatus::InvalidInput) return failure(error);
        if (status == LauncherCore::AppDiscoveryStatus::Unavailable) return operationFailure(error);
        QJsonArray apps;
        for (const auto &bundle : bundles) apps.append(bundle);
        return {{"ok", true}, {"exitCode", 0}, {"data", QJsonObject{{"apps", apps}}}};
    }
    return failure(arguments.contains("--mount-all") ? "Headless mode does not mount implicitly. An explicit mount command is required." : "Unknown command or option.");
}
namespace {
volatile sig_atomic_t signalWriteFd = -1;
void wakeSignal(int) { const int saved = errno; const char byte = 's'; if (signalWriteFd >= 0) { const auto ignored = ::write(int(signalWriteFd), &byte, 1); (void)ignored; } errno = saved; }
class Stream : public QObject {
    QCoreApplication &application;
    bool broken = false, ready = false;
    int outputFlags = -1, wake[2]{-1, -1}, status = 0;
    struct sigaction oldInt{}, oldTerm{}, oldPipe{};
    bool signalsInstalled = false;
    QSocketNotifier *output = nullptr, *signal = nullptr;
    QByteArray pendingOutput;
    void flush() {
        while (!pendingOutput.isEmpty() && !broken) {
            const auto written = ::write(STDOUT_FILENO, pendingOutput.constData(), pendingOutput.size());
            if (written > 0) pendingOutput.remove(0, written);
            else if (written < 0 && errno == EINTR) continue;
            else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            else { broken = true; pendingOutput.clear(); status = status == 0 ? 4 : status; }
        }
    }
    void service() {
        flush();
        if (output) output->setEnabled(!pendingOutput.isEmpty() && !broken);
        if (broken || pendingOutput.isEmpty()) application.exit(status);
    }
public:
    Stream(QCoreApplication &app, QByteArray initial, int code) : application(app), status(code), pendingOutput(std::move(initial)) {
        outputFlags = fcntl(STDOUT_FILENO, F_GETFL);
        if (outputFlags < 0 || fcntl(STDOUT_FILENO, F_SETFL, outputFlags | O_NONBLOCK) < 0 || pipe2(wake, O_NONBLOCK | O_CLOEXEC) != 0) return;
        struct sigaction action{}; sigemptyset(&action.sa_mask); action.sa_handler = wakeSignal;
        if (sigaction(SIGINT, &action, &oldInt) != 0) return;
        if (sigaction(SIGTERM, &action, &oldTerm) != 0) { sigaction(SIGINT, &oldInt, nullptr); return; }
        action.sa_handler = SIG_IGN;
        if (sigaction(SIGPIPE, &action, &oldPipe) != 0) { sigaction(SIGINT, &oldInt, nullptr); sigaction(SIGTERM, &oldTerm, nullptr); return; }
        signalsInstalled = true; signalWriteFd = wake[1];
        output = new QSocketNotifier(STDOUT_FILENO, QSocketNotifier::Write, this); output->setEnabled(false); connect(output, &QSocketNotifier::activated, this, [this] { service(); });
        signal = new QSocketNotifier(wake[0], QSocketNotifier::Read, this); connect(signal, &QSocketNotifier::activated, this, [this] { char bytes[64]; while (::read(wake[0], bytes, sizeof bytes) > 0) {} pendingOutput.clear(); application.exit(130); });
        ready = true; QTimer::singleShot(0, this, [this] { service(); });
    }
    ~Stream() override {
        signalWriteFd = -1;
        if (signalsInstalled) { sigaction(SIGINT, &oldInt, nullptr); sigaction(SIGTERM, &oldTerm, nullptr); sigaction(SIGPIPE, &oldPipe, nullptr); }
        if (outputFlags >= 0) fcntl(STDOUT_FILENO, F_SETFL, outputFlags);
        for (const int fd : wake) if (fd >= 0) ::close(fd);
    }
    int exec() { if (!ready) { fputs("Cannot configure nonblocking CLI output.\n", stderr); return 3; } return application.exec(); }
};
}
int run(QCoreApplication &application, const QStringList &arguments, const QString &error) {
    const auto result = error.isEmpty() ? dispatch(arguments) : failure(error);
    const bool textHelp = error.isEmpty() && helpRequest(arguments);
    Stream stream(application, textHelp ? helpText().toUtf8() : QJsonDocument(result).toJson(QJsonDocument::Compact) + '\n', result.value("exitCode").toInt());
    return stream.exec();
}
}
