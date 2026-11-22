// SPDX-License-Identifier: GPL-3.0-or-later
#include "cli.h"
#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QFile>
#include <QLibrary>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QtTest>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <csignal>

class CliTest : public QObject {
    Q_OBJECT
    QProcessEnvironment environment(const QString &home) {
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("HOME", home); env.insert("XDG_CONFIG_HOME", home + "/config"); env.insert("XDG_CACHE_HOME", home + "/cache"); env.insert("XDG_DATA_HOME", home + "/data");
        env.remove("DISPLAY"); env.remove("WAYLAND_DISPLAY"); env.remove("WAYLAND_SOCKET"); env.remove("DARLING_LAUNCHER_HEADLESS"); env.insert("QT_QPA_PLATFORM", "invalid-cli-fixture-platform");
        return env;
    }
private slots:
    void selection() {
        QProcessEnvironment env; int probes = 0;
        auto desktop = [&](QStringList *) { ++probes; return QString("xcb"); };
        QCOMPARE(LauncherCli::selectEntry({}, env, desktop).kind, LauncherCli::EntryKind::Gui); QCOMPARE(probes, 1);
        for (const auto &args : {QStringList{"--help"}, QStringList{"help"}, QStringList{"session"}, QStringList{"unknown"}, QStringList{"--headless"}}) { probes = 0; QCOMPARE(LauncherCli::selectEntry(args, env, desktop).kind, LauncherCli::EntryKind::Cli); QCOMPARE(probes, 0); }
        env.insert("DARLING_LAUNCHER_HEADLESS", "1"); probes = 0; QCOMPARE(LauncherCli::selectEntry({}, env, desktop).kind, LauncherCli::EntryKind::Cli); QCOMPARE(probes, 0);
        env.insert("DARLING_LAUNCHER_HEADLESS", "0"); QCOMPARE(LauncherCli::selectEntry({}, env, desktop).kind, LauncherCli::EntryKind::Gui);
        env.insert("DARLING_LAUNCHER_HEADLESS", "invalid"); probes = 0; QCOMPARE(LauncherCli::selectEntry({"help"}, env, desktop).kind, LauncherCli::EntryKind::Error); QCOMPARE(probes, 0);
        env.remove("DARLING_LAUNCHER_HEADLESS"); const auto mounted = LauncherCli::selectEntry({"--mount-all"}, env, desktop); QVERIFY(mounted.mountAll); QCOMPARE(mounted.kind, LauncherCli::EntryKind::Gui);
        const auto unavailable = LauncherCli::selectEntry({}, env, [](QStringList *) { return QString(); }); QCOMPARE(unavailable.kind, LauncherCli::EntryKind::Cli);
    }
    void startup_data() {
        QTest::addColumn<QString>("mode"); for (const auto &mode : {"none", "empty", "stale-x", "remote-x", "dead-wayland", "help", "flag", "env", "env-invalid", "unknown", "session-unsupported", "mount-headless", "apps-list", "apps-list-env", "apps-list-root-symlink", "apps-list-empty", "apps-list-invalid-root", "apps-list-missing-root", "apps-list-no-option", "apps-list-no-value", "apps-list-unknown-option", "apps-list-duplicate", "apps-list-extra", "apps-list-unrepresentable-name", "apps-list-unreadable-layout", "apps-list-unreadable-nested"}) QTest::newRow(mode) << QString(mode);
    }
    void startup() {
        QFETCH(QString, mode); QTemporaryDir home; QVERIFY(home.isValid()); auto env = environment(home.path()); QStringList arguments;
        if (mode == "empty") { env.insert("DISPLAY", ""); env.insert("WAYLAND_DISPLAY", ""); }
        if (mode == "stale-x") env.insert("DISPLAY", ":65530");
        if (mode == "remote-x") env.insert("DISPLAY", "192.0.2.1:0");
        if (mode == "dead-wayland") { env.insert("WAYLAND_DISPLAY", "missing-display"); env.insert("XDG_RUNTIME_DIR", home.path()); }
        if (mode == "help") arguments << "--help";
        if (mode == "flag") { arguments << "--headless"; env.insert("DISPLAY", ":0"); }
        if (mode == "env") { env.insert("DARLING_LAUNCHER_HEADLESS", "1"); env.insert("DISPLAY", ":0"); }
        if (mode == "env-invalid") env.insert("DARLING_LAUNCHER_HEADLESS", "");
        if (mode == "unknown") arguments << "apps" << "launch";
        if (mode == "session-unsupported") arguments << "session";
        if (mode == "mount-headless") arguments << "--headless" << "--mount-all";
        int rawAppsFd = -1; QByteArray rawName; bool rawNameCreated = false;
        QString restricted; mode_t restrictedMode = 0;
        const auto restorePermissions = qScopeGuard([&] { if (!restricted.isEmpty()) ::chmod(QFile::encodeName(restricted).constData(), restrictedMode); });
        const auto closeRawAppsFd = qScopeGuard([&] { if (rawAppsFd >= 0) ::close(rawAppsFd); });
        const auto removeRawName = qScopeGuard([&] { if (rawAppsFd >= 0 && rawNameCreated) ::unlinkat(rawAppsFd, rawName.constData(), AT_REMOVEDIR); });
        if (mode.startsWith("apps-list")) {
            const QString volume = home.path() + "/source";
            const bool permissionFixture = mode == "apps-list-unreadable-layout" || mode == "apps-list-unreadable-nested";
            if (mode == "apps-list" || mode == "apps-list-env" || mode == "apps-list-root-symlink" || mode == "apps-list-empty" || mode == "apps-list-unknown-option" || mode == "apps-list-duplicate" || mode == "apps-list-extra" || mode == "apps-list-unrepresentable-name" || permissionFixture) {
                QVERIFY(QDir().mkpath(volume + "/System/Library"));
                if (mode == "apps-list" || mode == "apps-list-env" || mode == "apps-list-root-symlink" || mode == "apps-list-unrepresentable-name" || permissionFixture) {
                    QVERIFY(QDir().mkpath(volume + "/Applications/Alpha.app"));
                    QVERIFY(QDir().mkpath(volume + "/System/Applications/Zeta.app"));
                } else QVERIFY(QDir().mkpath(volume + "/Applications"));
            } else if (mode == "apps-list-invalid-root") QVERIFY(QDir().mkpath(volume));
            QString selectedVolume = volume;
            if (mode == "apps-list-root-symlink") { selectedVolume = home.path() + "/source-link"; QVERIFY(QFile::link(volume, selectedVolume)); }
            if (permissionFixture) {
                const QString blocked = mode == "apps-list-unreadable-layout" ? volume + "/System" : volume + "/Applications/Restricted";
                if (mode == "apps-list-unreadable-nested") QVERIFY(QDir().mkpath(blocked + "/Hidden.app"));
                struct stat info{}; QCOMPARE(::stat(QFile::encodeName(blocked).constData(), &info), 0);
                restrictedMode = info.st_mode & 07777; restricted = blocked;
                QCOMPARE(::chmod(QFile::encodeName(restricted).constData(), 0), 0);
                if (::faccessat(AT_FDCWD, QFile::encodeName(restricted).constData(), R_OK | X_OK, AT_EACCESS) == 0)
                    QSKIP("Effective access still permits the restricted fixture directory");
                QCOMPARE(errno, EACCES);
            }
            if (mode == "apps-list-unrepresentable-name") {
                const QByteArray appsPath = QFile::encodeName(volume + "/Applications");
                rawAppsFd = ::open(appsPath.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                QVERIFY(rawAppsFd >= 0);
                rawName = QByteArray("Invalid\xff.app", 12);
                QVERIFY(::mkdirat(rawAppsFd, rawName.constData(), 0700) == 0);
                rawNameCreated = true;
            }
            if (mode == "apps-list-env") env.insert("DARLING_LAUNCHER_HEADLESS", "1");
            else arguments << "--headless";
            arguments << "apps" << "list";
            if (mode == "apps-list-no-value") arguments << "--volume";
            else if (mode != "apps-list-no-option") {
                arguments << (mode == "apps-list-unknown-option" ? "--source" : "--volume");
                arguments << (mode == "apps-list-missing-root" ? home.path() + "/missing" : selectedVolume);
            }
            if (mode == "apps-list-duplicate") arguments << "--volume" << volume;
            if (mode == "apps-list-extra") arguments << "extra";
        }
        const auto homeEntriesBefore = QDir(home.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        QProcess process; process.setProcessEnvironment(env); process.setWorkingDirectory(home.path()); process.start(LAUNCHER_EXECUTABLE, arguments);
        QVERIFY(process.waitForFinished(10000)); QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        const bool appList = mode.startsWith("apps-list");
        const bool appListSuccess = mode == "apps-list" || mode == "apps-list-env" || mode == "apps-list-root-symlink" || mode == "apps-list-empty";
        const bool appListOperationalFailure = mode == "apps-list-unrepresentable-name" || mode == "apps-list-unreadable-layout" || mode == "apps-list-unreadable-nested";
        const bool rejected = mode == "env-invalid" || mode == "unknown" || mode == "session-unsupported" || mode == "mount-headless";
        const int expectedExit = appList ? (appListSuccess ? 0 : (appListOperationalFailure ? 3 : 2)) : (rejected ? 2 : 0);
        QCOMPARE(process.exitCode(), expectedExit); const auto output = process.readAllStandardOutput();
        if (appList) {
            const auto document = QJsonDocument::fromJson(output); QVERIFY(document.isObject());
            const auto result = document.object();
            QVERIFY(result.value("ok").isBool()); QVERIFY(result.value("exitCode").isDouble());
            QCOMPARE(result.value("ok").toBool(), appListSuccess);
            QCOMPARE(result.value("exitCode").toInt(-1), expectedExit);
            if (appListSuccess) {
                QVERIFY(result.value("data").isObject());
                const auto data = result.value("data").toObject(); QVERIFY(data.value("apps").isArray());
                QStringList bundles;
                for (const auto &value : data.value("apps").toArray()) bundles << value.toString();
                const QStringList expected = mode == "apps-list-empty" ? QStringList{} : QStringList{"Applications/Alpha.app", "System/Applications/Zeta.app"};
                QCOMPARE(bundles, expected);
            } else { QCOMPARE(result.value("error").toObject().value("code").toString(), appListOperationalFailure ? QString("source-unavailable") : QString("invalid-input")); QVERIFY(!result.contains("data")); }
        } else if (rejected) { const auto document = QJsonDocument::fromJson(output); QVERIFY(document.isObject()); const auto result = document.object(); QVERIFY(result.value("ok").isBool()); QVERIFY(result.value("exitCode").isDouble()); QVERIFY(!result.value("ok").toBool()); QCOMPARE(result.value("exitCode").toInt(-1), expectedExit); QCOMPARE(result.value("error").toObject().value("code").toString(), QString("invalid-input")); }
        else QVERIFY(output.startsWith("Usage: darling-launcher"));
        QVERIFY(!process.readAllStandardError().contains("Could not load the Qt platform plugin"));
        QCOMPARE(QDir(home.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot), homeEntriesBefore);
    }
    void silentDisplay_data() { QTest::addColumn<bool>("stopParent"); QTest::newRow("named-display-poisoned-socket") << false; QTest::newRow("parent-exit") << true; }
    void silentDisplay() {
        QFETCH(bool, stopParent); QLibrary wayland("libwayland-client.so.0"); if (!wayland.load()) QSKIP("Wayland client library absent; startup fallback fixtures still apply.");
        QTemporaryDir home("/tmp/darling-launcher-display-XXXXXX"); QVERIFY2(home.isValid(), qPrintable(home.errorString())); QLocalServer server;
        const QString socketPath = home.path() + "/display"; const auto socketBytes = QFile::encodeName(socketPath);
        auto diagnostic = [&] { return QString("Qt temp path: %1; socket path: %2; encoded length: %3; server error: %4").arg(QDir::tempPath(), socketPath).arg(socketBytes.size()).arg(server.errorString()); };
        QVERIFY2(socketBytes.size() + 2 <= qsizetype(sizeof(sockaddr_un{}.sun_path)), qPrintable(diagnostic()));
        QVERIFY2(server.listen(socketPath), qPrintable(diagnostic()));
        auto env = environment(home.path()); env.insert("WAYLAND_DISPLAY", "display"); env.insert("WAYLAND_SOCKET", "999999"); env.insert("XDG_RUNTIME_DIR", home.path());
        QProcess process; auto stop = qScopeGuard([&] { if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(2000); } }); process.setProcessEnvironment(env); process.start(LAUNCHER_EXECUTABLE, QStringList{}); QVERIFY(process.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 3000); auto *connection = server.nextPendingConnection(); QVERIFY(connection); connection->setParent(&server);
        QFile children("/proc/" + QString::number(process.processId()) + "/task/" + QString::number(process.processId()) + "/children"); QVERIFY(children.open(QIODevice::ReadOnly)); const QString child = QString::fromUtf8(children.readAll()).trimmed().section(' ', 0, 0); QVERIFY(!child.isEmpty());
        if (stopParent) { process.kill(); QVERIFY(process.waitForFinished(2000)); }
        else { QVERIFY(process.waitForFinished(5000)); QCOMPARE(process.exitCode(), 0); QVERIFY(process.readAllStandardOutput().startsWith("Usage: darling-launcher")); }
        auto ended = [&] { QFile status("/proc/" + child + "/status"); return !status.open(QIODevice::ReadOnly) || status.readAll().contains("State:\tZ"); }; QTRY_VERIFY_WITH_TIMEOUT(ended(), 4500);
    }
    void directOutput_data() { QTest::addColumn<QString>("mode"); for (const auto &mode : {"drain", "sigint", "sigterm", "broken"}) QTest::newRow(mode) << QString(mode); }
    void directOutput() {
        QFETCH(QString, mode); QTemporaryDir home; QVERIFY(home.isValid());
        int pipeFds[2]; QCOMPARE(::pipe2(pipeFds, O_CLOEXEC | O_NONBLOCK), 0);
        auto close = qScopeGuard([&] { for (const int fd : pipeFds) if (fd >= 0) ::close(fd); });
        const int capacity = fcntl(pipeFds[0], F_SETPIPE_SZ, 4096); QVERIFY(capacity > 0);
        const QByteArray filler(capacity, 'x');
        if (mode == "broken") { ::close(pipeFds[0]); pipeFds[0] = -1; }
        else QCOMPARE(::write(pipeFds[1], filler.constData(), filler.size()), filler.size());
        const int writerFlags = fcntl(pipeFds[1], F_GETFL); QVERIFY(writerFlags >= 0); QCOMPARE(fcntl(pipeFds[1], F_SETFL, writerFlags & ~O_NONBLOCK), 0);
        QProcess process; auto stop = qScopeGuard([&] { if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(2000); } });
        process.setProcessEnvironment(environment(home.path())); process.setChildProcessModifier([&] { if (pipeFds[1] < 0 || ::dup2(pipeFds[1], STDOUT_FILENO) < 0) _exit(127); if (pipeFds[0] >= 0) ::close(pipeFds[0]); if (pipeFds[1] != STDOUT_FILENO) ::close(pipeFds[1]); });
        process.start(LAUNCHER_EXECUTABLE, {"--help"}); QVERIFY(process.waitForStarted()); ::close(pipeFds[1]); pipeFds[1] = -1;
        if (mode == "broken") { QVERIFY(process.waitForFinished(5000)); QCOMPARE(process.exitStatus(), QProcess::NormalExit); QCOMPARE(process.exitCode(), 4); return; }
        QTest::qWait(250); QCOMPARE(process.state(), QProcess::Running);
        if (mode == "sigint") QCOMPARE(::kill(process.processId(), SIGINT), 0); else if (mode == "sigterm") process.terminate();
        if (mode != "drain") QTest::qWait(100);
        QByteArray bytes; QElapsedTimer clock; clock.start();
        while (clock.elapsed() < 5000) { char buffer[8192]; const auto count = ::read(pipeFds[0], buffer, sizeof buffer); if (count > 0) bytes.append(buffer, count); QTest::qWait(5); if (process.state() == QProcess::NotRunning && count == 0) break; }
        QCOMPARE(process.state(), QProcess::NotRunning); QCOMPARE(process.exitStatus(), QProcess::NormalExit); QCOMPARE(process.exitCode(), mode == "drain" ? 0 : 130);
        QVERIFY(bytes.startsWith(filler)); bytes.remove(0, capacity);
        if (mode == "drain") QCOMPARE(bytes, LauncherCli::helpText().toUtf8());
    }
};
QTEST_GUILESS_MAIN(CliTest)
#include "cli_test.moc"
