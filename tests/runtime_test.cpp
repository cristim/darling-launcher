// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"
#include "runtimedownload.h"
#include "releases.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>
#include <unistd.h>

class RuntimeTest : public QObject {
    Q_OBJECT
    QTemporaryDir home;
    QByteArray originalPath;
    static bool write(const QString &path, const QByteArray &bytes, bool executable = false) {
        QFile file(path); if (!file.open(QIODevice::WriteOnly)) return false;
        if (file.write(bytes) != bytes.size()) return false;
        file.close(); return !executable || file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
private slots:
    void initTestCase() {
        QVERIFY(home.isValid()); qputenv("HOME", home.path().toUtf8()); qputenv("XDG_CONFIG_HOME", (home.path() + "/config").toUtf8());
        originalPath = qgetenv("PATH");
        QVERIFY(write(home.path() + "/gh", "#!/bin/sh\nprintf '%s\\n' \"$*\" >> \"$HOME/attestation-calls\"\nexit 0\n", true));
        qputenv("PATH", home.path().toUtf8() + ':' + originalPath);
    }
    void cleanupTestCase() { qputenv("PATH", originalPath); }
    void download_data() {
        QTest::addColumn<QString>("mode");
        for (const auto &mode : {"resume-disconnect", "resume-redirect", "resume-cancel", "resume-concurrent", "resume-ignored", "resume-mismatch", "resume-truncated", "resume-oversize", "resume-encoding", "resume-zero206", "resume-identity", "resume-complete", "resume-attestation", "resume-corrupt", "resume-symlink", "resume-hardlink", "resume-lock-link", "resume-lock-hardlink", "resume-directory-link"}) QTest::newRow(mode) << QString(mode);
        for (const auto &mode : {"success", "window", "window-cancel", "window-failure", "window-none", "window-shutdown-failure", "window-shutdown-hang", "window-shutdown-start", "window-init-hang", "window-init-start", "window-late", "window-deadline", "window-deadline-failure", "window-close", "window-destroy", "window-overlap", "none", "schema", "architecture", "checksum", "hash", "size", "oversize", "http", "cancel", "attestation", "verify-cancel", "host", "glibc", "redirect", "owner-spoof", "parent-symlink", "unpacked", "install-cancel", "redirect-valid", "redirect-relative", "redirect-stream", "library-cap", "library-start", "library-cancel"}) QTest::newRow(mode) << QString(mode);
    }
    void download() {
        QFETCH(QString, mode);
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); const QString root = temporary.path();
        const auto pathBefore = qgetenv("PATH"); auto restorePath = qScopeGuard([&] { qputenv("PATH", pathBefore); });
        if (mode.startsWith("library-")) { QVERIFY(write(root + "/ldconfig", mode == "library-cap" ? "#!/bin/sh\nexec python3 -c 'import sys,time; [(sys.stderr.write(\"x\" * (2 << 20)), sys.stderr.flush(), time.sleep(.15)) for _ in range(5)]'\n" : mode == "library-start" ? "#!/missing-fixture-interpreter\n" : "#!/bin/sh\nexec /usr/bin/sleep 30\n", true)); qputenv("PATH", root.toUtf8() + ':' + pathBefore); }
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        const QString base = "http://127.0.0.1:" + QString::number(server.serverPort());
        const QString tag = "v2026.10.14-aaaaaaa", name = "darling-runtime-" + tag + "-linux-" + LauncherReleases::hostArchitecture() + ".tar.zst";
        QVERIFY(QDir().mkpath(root + "/image/usr/local/bin")); QVERIFY(QDir().mkpath(root + "/image/usr/local/libexec/darling/private/etc"));
        QVERIFY(write(root + "/image/usr/local/bin/darling", mode == "window-cancel"
            ? "#!/bin/sh\nif [ \"$1\" = shutdown ]; then echo $$ > \"$DPREFIX/shutdown-pid\"; touch \"$DPREFIX/shutdown\"; exit 0; fi\necho $$ > \"$DPREFIX/init-pid\"; touch \"$DPREFIX/initializing\"\nwhile [ ! -e \"$DPREFIX/shutdown\" ]; do /usr/bin/sleep 0.05; done\nexit 0\n"
            : "#!/bin/sh\nif [ -e \"$HOME/fixture-initialize-fail\" ]; then exit 1; fi\nprintf '%s\\n' \"$DARLING_INSTALL_PREFIX\" > \"$DPREFIX/runtime-used\"\nexit 0\n", true));
        const bool teardown = mode.startsWith("window-shutdown-") || QStringList{"window-init-hang", "window-late", "window-deadline", "window-deadline-failure", "window-close", "window-destroy", "window-overlap"}.contains(mode);
        if (teardown) {
            const bool late = mode == "window-late" || mode == "window-overlap" || mode == "window-deadline-failure";
            const QByteArray shutdown = mode == "window-shutdown-hang" || mode == "window-close" || mode == "window-destroy" ? "exec /usr/bin/sleep 30\n" : mode == "window-shutdown-failure" ? "exit 1\n" : late ? "touch \"$DPREFIX/shutdown\"; /usr/bin/sleep .2; exit 1\n" : "exit 0\n";
            const QByteArray init = late ? "while [ ! -e \"$DPREFIX/shutdown\" ]; do /usr/bin/sleep .01; done\nexit 0\n" : "exec /usr/bin/sleep 30\n";
            QVERIFY(write(root + "/image/usr/local/bin/darling", "#!/bin/sh\nif [ \"$1\" = shutdown ]; then echo $$ > \"$DPREFIX/shutdown-pid\"; echo x >> \"$DPREFIX/shutdown-calls\"; " + shutdown + "fi\necho $$ > \"$DPREFIX/init-pid\"; touch \"$DPREFIX/initializing\"\n" + (mode == "window-shutdown-start" ? "chmod 600 \"$0\"\n" : "") + init, true));
        }
        if (mode == "window-init-start") QVERIFY(write(root + "/image/usr/local/bin/darling", "#!/missing-fixture-interpreter\n", true));
        QProcess tar;
        if (mode == "owner-spoof") {
            const QString script = "import io,tarfile,sys\nwith tarfile.open(sys.argv[1], 'w') as a:\n t=tarfile.TarInfo(sys.argv[2]+'/outside');t.gname='g 1 2026-10-09 14:32 usr/zz';t.size=1;a.addfile(t,io.BytesIO(b'x'))\n";
            QCOMPARE(QProcess::execute("python3", {"-c", script, root + "/spoof.tar", root}), 0);
            tar.start("zstd", {"-q", root + "/spoof.tar", "-o", root + "/fixture.tar.zst"});
        } else {
            if (mode == "parent-symlink") { QVERIFY(QDir().rename(root + "/image/usr/local/bin", root + "/image/usr/local/real-bin")); QVERIFY(QFile::link("real-bin", root + "/image/usr/local/bin")); }
            tar.start("tar", {"--zstd", "-cf", root + "/fixture.tar.zst", "-C", root + "/image", "usr"});
        }
        QVERIFY(tar.waitForFinished()); QCOMPARE(tar.exitCode(), 0);
        QFile archive(root + "/fixture.tar.zst"); QVERIFY(archive.open(QIODevice::ReadOnly)); const auto archiveBytes = archive.readAll();
        const auto digest = QCryptographicHash::hash(archiveBytes, QCryptographicHash::Sha256).toHex();
        QJsonArray assets;
        for (const auto &file : QStringList{"manifest.json", "SHA256SUMS", "refs.lock.json", "nested.refs.lock.json", name}) {
            QJsonObject object{{"name", file}, {"browser_download_url", QString("https://github.com/") + file}};
            if (file == name) { object["size"] = archiveBytes.size(); object["digest"] = "sha256:" + QString::fromLatin1(digest); }
            assets.append(object);
        }
        QJsonObject release{{"tag_name", tag}, {"published_at", "2026-10-14T10:00:00Z"}, {"assets", assets}};
        QJsonObject artifact{{"file", name}, {"url", QString("https://github.com/") + name}, {"sha256", QString::fromLatin1(digest)}, {"size", archiveBytes.size() + (mode == "size" ? 1 : 0)}, {"unpacked_size", 4096}, {"install_root", "usr/local"}, {"launcher", "usr/local/bin/darling"}, {"host_requires", QJsonObject{{"glibc", "1.0"}, {"wrapped_sonames", mode == "host" ? QJsonArray{"lib-launcher-fixture-missing.so.999"} : QJsonArray{}}}}};
        QJsonObject manifest{{"schema", mode == "schema" ? 2 : 1}, {"version", tag}, {"artifacts", QJsonObject{{mode == "architecture" ? "absent" : LauncherReleases::hostArchitecture(), artifact}}}};
        if (mode == "unpacked") { artifact["unpacked_size"] = 1; manifest["artifacts"] = QJsonObject{{LauncherReleases::hostArchitecture(), artifact}}; }
        if (mode == "glibc") { artifact["host_requires"] = QJsonObject{{"glibc", "garbage"}, {"wrapped_sonames", QJsonArray{}}}; manifest["artifacts"] = QJsonObject{{LauncherReleases::hostArchitecture(), artifact}}; }
        QMap<QString, QByteArray> responses{{"/manifest.json", QJsonDocument(manifest).toJson()}, {"/SHA256SUMS", (mode == "checksum" ? QByteArray(64, '0') : digest) + "  " + name.toUtf8() + '\n'}, {'/' + name, archiveBytes}};
        bool identityRequested = true; int redirects = 0, archiveRequests = 0; QStringList ranges;
        QString redirectPartial; bool redirectRetainedBytes = false;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                    const auto request = socket->readAll(); const auto path = QString::fromLatin1(request.split(' ').value(1));
                    identityRequested &= request.contains("Accept-Encoding: identity\r\n");
                    if (mode.startsWith("resume-") && path.startsWith('/' + name)) {
                        ++archiveRequests;
                        const auto range = QRegularExpression("(?im)^Range: bytes=([0-9]+)-\\r?$").match(QString::fromLatin1(request)); ranges << (range.hasMatch() ? range.captured(1) : QString());
                        const qint64 offset = range.hasMatch() ? range.captured(1).toLongLong() : 0, half = archiveBytes.size() / 2;
                        QByteArray payload = archiveBytes.mid(offset), extra; int status = offset ? 206 : 200; qint64 declared = payload.size();
                        if (archiveRequests == 1 && mode != "resume-complete" && mode != "resume-attestation" && mode != "resume-corrupt" && mode != "resume-zero206") {
                            socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(archiveBytes.size()) + "\r\n\r\n" + archiveBytes.left(half));
                            if (mode != "resume-cancel" && mode != "resume-concurrent") socket->disconnectFromHost(); return;
                        }
                        if (mode == "resume-redirect" && archiveRequests == 2) { socket->write("HTTP/1.1 302 Found\r\nLocation: /" + name.toUtf8() + "?resumed=1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"); socket->disconnectFromHost(); return; }
                        if (mode == "resume-redirect" && archiveRequests == 3) { QFile retained(redirectPartial); redirectRetainedBytes = retained.open(QIODevice::ReadOnly) && retained.readAll() == archiveBytes.left(half); }
                        if (archiveRequests == 1 && mode == "resume-corrupt") payload[0] = static_cast<char>(payload[0] ^ 1);
                        if (archiveRequests == 1 && mode == "resume-zero206") status = 206;
                        if (archiveRequests == 2 && mode == "resume-ignored") { status = 200; payload = archiveBytes; declared = payload.size(); }
                        if (status == 206) extra = "Content-Range: bytes " + QByteArray::number(offset + (archiveRequests == 2 && mode == "resume-mismatch" ? 1 : 0)) + '-' + QByteArray::number(archiveBytes.size() - 1) + '/' + QByteArray::number(archiveBytes.size()) + "\r\n";
                        if (archiveRequests == 2 && mode == "resume-truncated") payload = payload.left(payload.size() / 2);
                        if (archiveRequests == 2 && mode == "resume-oversize") { payload += "excess"; declared = payload.size(); }
                        if (archiveRequests == 2 && mode == "resume-encoding") extra += "Content-Encoding: gzip\r\n";
                        socket->write(QByteArray(status == 206 ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n") + extra + "Content-Length: " + QByteArray::number(declared) + "\r\nConnection: close\r\n\r\n" + payload); socket->disconnectFromHost(); return;
                    }
                    if (mode == "cancel") return;
                    if (mode.startsWith("redirect-") && path == "/manifest.json") {
                        ++redirects; socket->write("HTTP/1.1 302 Found\r\nLocation: " + QByteArray(mode == "redirect-valid" ? "https://github.com/manifest-final" : "/manifest-final") + "\r\nContent-Length: 1000000000\r\n\r\n");
                        auto *stream = new QTimer(socket); connect(stream, &QTimer::timeout, socket, [socket] { socket->write(QByteArray(32768, 'x')); }); connect(socket, &QTcpSocket::disconnected, stream, &QTimer::stop); stream->start(50); return;
                    }
                    QByteArray body;
                    if (path.startsWith("/repos/")) body = QJsonDocument(mode.endsWith("none") ? QJsonArray{QJsonObject{{"tag_name", "v0.1.20260921"}}} : QJsonArray{release}).toJson();
                    else body = responses.value(path == "/manifest-final" ? "/manifest.json" : path);
                    if (mode == "redirect") { socket->write("HTTP/1.1 302 Found\r\nLocation: https://evil.invalid/runtime\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"); socket->disconnectFromHost(); return; }
                    if (mode == "oversize" && path == '/' + name) body += "excess";
                    if (mode == "hash" && path == '/' + name) body[0] = static_cast<char>(body[0] ^ 1);
                    socket->write(QByteArray(mode == "http" ? "HTTP/1.1 503 Unavailable\r\n" : "HTTP/1.1 200 OK\r\n") + "Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body); socket->disconnectFromHost();
                });
            }
        });
        QVERIFY(write(home.path() + "/attestation-calls", {}));
        QVERIFY(write(home.path() + "/gh", mode == "attestation" ? "#!/bin/sh\necho rejected >&2\nexit 1\n" : mode == "verify-cancel" ? "#!/bin/sh\nexec /usr/bin/sleep 30\n" : "#!/bin/sh\nprintf '%s\\n' \"$*\" >> \"$HOME/attestation-calls\"\nexit 0\n", true));
        if (mode.startsWith("window")) {
            const QString windowHome = root + "/home"; QVERIFY(QDir().mkpath(windowHome));
            qputenv("HOME", windowHome.toUtf8()); auto restore = qScopeGuard([this] { qputenv("HOME", home.path().toUtf8()); });
            QSettings("cristim", "darling-launcher").clear();
            auto ownedWindow = std::make_unique<Window>(QString(), false, [] { return QList<SourceMount>{}; }, QUrl(base), mode.startsWith("window-deadline") ? 200 : 60000, 400); Window &window = *ownedWindow; window.show();
            window.findChild<QLineEdit *>("darlingField")->clear(); window.findChild<QLineEdit *>("runtimeRootField")->clear();
            if (mode == "window-overlap") { window.findChild<QPushButton *>("Create prefix")->click(); QVERIFY(window.findChild<QDialog *>("prefixBuildDialog")); window.findChild<QPushButton *>("setupSelectRuntime")->click(); QVERIFY(window.findChild<QDialog *>("existingDarling")); QVERIFY(QDir().mkpath(windowHome + "/.darling-launcher/prefixes/prebuilt")); QVERIFY(write(windowHome + "/.darling-launcher/prefixes/prebuilt/keep", "collision")); }
            window.offerRuntimeSetup(); QVERIFY(window.findChild<QDialog *>("runtimeDownloadDialog"));
            if (mode == "window-init-start") { QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QLabel *>("runtimeDownloadStatus")->text().contains("could not initialize"), 10000); QVERIFY(window.findChild<QLineEdit *>("darlingField")->text().isEmpty()); QVERIFY(QSettings("cristim", "darling-launcher").value("darling").toString().isEmpty()); return; }
            if (teardown) {
                const QString newPrefix = windowHome + "/.darling-launcher/prefixes/prebuilt" + (mode == "window-overlap" ? "-2" : "");
                QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(newPrefix + "/initializing"), 10000);
                QFile pidFile(newPrefix + "/init-pid"); QVERIFY(pidFile.open(QIODevice::ReadOnly)); const QString initPid = QString::fromUtf8(pidFile.readAll()).trimmed();
                if (mode == "window-destroy") { QElapsedTimer clock; clock.start(); ownedWindow.reset(); QVERIFY(clock.elapsed() < 2000); }
                else {
                    if (!mode.startsWith("window-deadline")) window.findChild<QPushButton *>("runtimeDownloadCancel")->click();
                    QVERIFY(!window.findChild<QPushButton *>("runtimeBuildSource")->isEnabled()); QVERIFY(!window.findChild<QPushButton *>("runtimeDownloadRetry")->isEnabled());
                    window.findChild<QPushButton *>("runtimeDownloadRetry")->click(); window.findChild<QPushButton *>("Create prefix")->click();
                    const int wrappers = window.findChildren<QProcess *>().size(); window.findChild<QPushButton *>("Initialize selected prefix")->click(); QCOMPARE(window.findChildren<QProcess *>().size(), wrappers);
                    window.findChild<QPushButton *>("setupSelectRuntime")->click();
                    if (mode == "window-overlap") { QVERIFY(!window.findChild<QDialog *>("prefixBuildDialog")->isEnabled()); window.findChild<QPushButton *>("buildPrefix")->click(); QVERIFY(!window.findChild<QPushButton *>("buildPrefix")->isEnabled()); window.findChild<QPushButton *>("useExistingDarling")->click(); QVERIFY(window.findChild<QLabel *>("existingMessage")->text().contains("Wait for runtime setup")); QVERIFY(QFileInfo::exists(windowHome + "/.darling-launcher/prefixes/prebuilt/keep")); }
                    else { QVERIFY(!window.findChild<QDialog *>("prefixBuildDialog")); QVERIFY(!window.findChild<QDialog *>("existingDarling")); }
                    if (mode == "window-close") { window.close(); QVERIFY(window.isVisible()); }
                    QTRY_VERIFY_WITH_TIMEOUT(!window.findChild<QDialog *>("runtimeDownloadDialog")->property("runtimeInitializationPending").toBool(), 3000);
                    const QString diagnostic = window.findChild<QLabel *>("runtimeDownloadStatus")->text(); QVERIFY2(diagnostic.contains("manual guest shutdown") || diagnostic.contains("timed out"), qPrintable(diagnostic));
                    if (mode == "window-deadline-failure") { QVERIFY(diagnostic.contains("initialization timed out")); QVERIFY(diagnostic.contains("Prefix shutdown failed")); }
                    QVERIFY(window.findChild<QLineEdit *>("darlingField")->text().isEmpty()); QVERIFY(window.findChild<QLineEdit *>("runtimeRootField")->text().isEmpty());
                }
                QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists("/proc/" + initPid), 2000);
                QFile shutdownPid(newPrefix + "/shutdown-pid"); if (shutdownPid.open(QIODevice::ReadOnly)) { const QString pid = QString::fromUtf8(shutdownPid.readAll()).trimmed(); QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists("/proc/" + pid), 2000); }
                QFile calls(newPrefix + "/shutdown-calls"); if (calls.open(QIODevice::ReadOnly)) QCOMPARE(calls.readAll(), QByteArray("x\n"));
                QVERIFY(QFileInfo::exists(newPrefix)); QVERIFY(QSettings("cristim", "darling-launcher").value("darling").toString().isEmpty()); QVERIFY(QSettings("cristim", "darling-launcher").value("runtimeRoot").toString().isEmpty()); return;
            }
            if (mode == "window-none") {
                QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QLabel *>("runtimeDownloadStatus")->text().contains("No stable prebuilt"), 5000);
                QVERIFY(!window.findChild<QDialog *>("prefixBuildDialog"));
                window.findChild<QPushButton *>("runtimeBuildSource")->click(); QVERIFY(window.findChild<QDialog *>("prefixBuildDialog"));
                return;
            }
            if (mode == "window-failure") {
                QVERIFY(write(windowHome + "/fixture-initialize-fail", "fixture"));
                QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QLabel *>("runtimeDownloadStatus")->text().contains("could not initialize"), 10000);
                QVERIFY(window.findChild<QLineEdit *>("darlingField")->text().isEmpty());
                QVERIFY(window.findChild<QLineEdit *>("runtimeRootField")->text().isEmpty());
                QVERIFY(QFile::remove(windowHome + "/fixture-initialize-fail"));
                window.findChild<QPushButton *>("runtimeDownloadRetry")->click();
            }
            if (mode == "window-cancel") {
                const QString newPrefix = windowHome + "/.darling-launcher/prefixes/prebuilt";
                QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(newPrefix + "/initializing"), 10000);
                window.findChild<QPushButton *>("runtimeDownloadCancel")->click();
                const bool sourceDisabled = !window.findChild<QPushButton *>("runtimeBuildSource")->isEnabled(), retryDisabled = !window.findChild<QPushButton *>("runtimeDownloadRetry")->isEnabled();
                const bool pending = window.findChild<QDialog *>("runtimeDownloadDialog")->property("runtimeInitializationPending").toBool();
                QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(newPrefix + "/shutdown"), 5000);
                QTRY_VERIFY_WITH_TIMEOUT(!window.findChild<QDialog *>("runtimeDownloadDialog")->property("runtimeInitializationPending").toBool(), 5000);
                for (const auto &name : {"init-pid", "shutdown-pid"}) { QFile pid(newPrefix + '/' + name); QVERIFY(pid.open(QIODevice::ReadOnly)); const QString process = QString::fromUtf8(pid.readAll()).trimmed(); QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists("/proc/" + process), 2000); }
                QVERIFY(sourceDisabled); QVERIFY(retryDisabled); QVERIFY(pending);
                QVERIFY(window.findChild<QLineEdit *>("darlingField")->text().isEmpty());
                QVERIFY(window.findChild<QLineEdit *>("runtimeRootField")->text().isEmpty());
                return;
            }
            QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(window.findChild<QLineEdit *>("prefixField")->text() + "/runtime-used"), 10000);
            const QString runtime = window.findChild<QLineEdit *>("runtimeRootField")->text();
            QCOMPARE(runtime, windowHome + "/.darling-launcher/runtimes/" + tag + "/usr/local");
            QCOMPARE(window.findChild<QLineEdit *>("darlingField")->text(), runtime + "/bin/darling");
            QFile used(window.findChild<QLineEdit *>("prefixField")->text() + "/runtime-used"); QVERIFY(used.open(QIODevice::ReadOnly)); QCOMPARE(used.readAll().trimmed(), runtime.toUtf8());
            QTRY_VERIFY(!window.findChild<QWidget *>("setupBanner")->isVisible());
            return;
        }
        if (mode.startsWith("resume-")) {
            if (mode == "resume-attestation") QVERIFY(write(home.path() + "/gh", "#!/bin/sh\ncase \"$3\" in */runtime.tar.zst) exit 1;; esac\nexit 0\n", true));
            RuntimeDownload first(nullptr, QUrl(base)); QSignalSpy firstDone(&first, &RuntimeDownload::completed), firstProgress(&first, &RuntimeDownload::progress);
            if (mode == "resume-cancel" || mode == "resume-complete") connect(&first, &RuntimeDownload::progress, &first, [&](const QString &phase, qint64 received) { if ((mode == "resume-cancel" && phase == "Downloading runtime" && received > 0) || (mode == "resume-complete" && phase == "Verifying and installing runtime")) first.cancel(); }, Qt::DirectConnection);
            first.start(root + "/data");
            if (mode == "resume-concurrent") {
                QTRY_VERIFY_WITH_TIMEOUT(!firstProgress.isEmpty() && firstProgress.last()[0].toString() == "Downloading runtime" && firstProgress.last()[1].toLongLong() > 0, 10000);
                RuntimeDownload competing(nullptr, QUrl(base)); QSignalSpy competingDone(&competing, &RuntimeDownload::completed); competing.start(root + "/data"); QTRY_COMPARE_WITH_TIMEOUT(competingDone.size(), 1, 10000);
                QVERIFY(competingDone.first()[1].toString().contains("already running")); QCOMPARE(archiveRequests, 1); first.cancel();
            }
            QTRY_COMPARE_WITH_TIMEOUT(firstDone.size(), 1, 10000); QVERIFY(!firstDone.first()[1].toString().isEmpty()); QVERIFY(!QFileInfo::exists(root + "/data/runtimes/" + tag));
            if (mode == "resume-attestation") QVERIFY(write(home.path() + "/gh", "#!/bin/sh\nexit 0\n", true));
            const QString tagCache = root + "/data/downloads/" + tag; const auto identities = QDir(tagCache).entryList(QDir::Dirs | QDir::NoDotAndDotDot); QCOMPARE(identities.size(), 1);
            const QString cache = tagCache + '/' + identities.first(), part = cache + "/archive.part";
            const qint64 half = archiveBytes.size() / 2;
            if (mode != "resume-corrupt" && mode != "resume-zero206") { QFile file(part); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), (mode == "resume-complete" || mode == "resume-attestation") ? archiveBytes : archiveBytes.left(half)); }
            else QVERIFY(!QDir(cache).entryList({"retained-*.part"}, QDir::Files).isEmpty());
            if (mode == "resume-symlink" || mode.endsWith("hardlink") || mode == "resume-lock-link" || mode == "resume-directory-link") {
                const QString original = mode == "resume-directory-link" ? cache : mode.startsWith("resume-lock-") ? cache + "/archive.lock" : part;
                QVERIFY(QDir().rename(original, root + "/retained-fixture"));
                if (mode == "resume-directory-link") QVERIFY(QFile::link(root + "/retained-fixture", original)); else { QVERIFY(write(root + "/victim", "keep")); QVERIFY(QFile::setPermissions(root + "/victim", QFile::ReadOwner | QFile::WriteOwner)); if (mode.endsWith("hardlink")) QCOMPARE(::link(QFile::encodeName(root + "/victim").constData(), QFile::encodeName(original).constData()), 0); else QVERIFY(QFile::link(root + "/victim", original)); }
            }
            if (mode == "resume-identity") {
                artifact["url"] = artifact.value("url").toString() + "?identity=2"; manifest["artifacts"] = QJsonObject{{LauncherReleases::hostArchitecture(), artifact}}; responses["/manifest.json"] = QJsonDocument(manifest).toJson();
                for (int i = 0; i < assets.size(); ++i) { auto item = assets[i].toObject(); if (item.value("name") == name) { item["browser_download_url"] = artifact.value("url"); assets[i] = item; } } release["assets"] = assets;
            }
            redirectPartial = part;
            RuntimeDownload second(nullptr, QUrl(base)); QSignalSpy secondDone(&second, &RuntimeDownload::completed); second.start(root + "/data"); QTRY_COMPARE_WITH_TIMEOUT(secondDone.size(), 1, 10000);
            if (mode.endsWith("symlink") || mode.endsWith("-link") || mode.endsWith("hardlink")) { QVERIFY(!secondDone.first()[1].toString().isEmpty()); QCOMPARE(archiveRequests, 1); QVERIFY(!QFileInfo::exists(root + "/data/runtimes/" + tag)); if (mode != "resume-directory-link") { QFile victim(root + "/victim"); QVERIFY(victim.open(QIODevice::ReadOnly)); QCOMPARE(victim.readAll(), QByteArray("keep")); } return; }
            const bool rejected = QStringList{"resume-mismatch", "resume-truncated", "resume-oversize", "resume-encoding"}.contains(mode);
            if (rejected) {
                QVERIFY(!secondDone.first()[1].toString().isEmpty()); QVERIFY(!QFileInfo::exists(root + "/data/runtimes/" + tag));
                if (mode == "resume-truncated") { QFile file(part); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), archiveBytes.left(half + (archiveBytes.size() - half) / 2)); }
                else { QVERIFY(!QFileInfo::exists(part)); const auto retained = QDir(cache).entryList({"retained-*.part"}, QDir::Files); QCOMPARE(retained.size(), 1); QFile file(cache + '/' + retained.first()); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), archiveBytes.left(half)); }
                RuntimeDownload recovered(nullptr, QUrl(base)); QSignalSpy recoveredDone(&recovered, &RuntimeDownload::completed); recovered.start(root + "/data"); QTRY_COMPARE_WITH_TIMEOUT(recoveredDone.size(), 1, 10000); QVERIFY2(recoveredDone.first()[1].toString().isEmpty(), qPrintable(recoveredDone.first()[1].toString()));
            } else QVERIFY2(secondDone.first()[1].toString().isEmpty(), qPrintable(secondDone.first()[1].toString()));
            QCOMPARE(ranges.first(), QString());
            if (mode == "resume-complete" || mode == "resume-attestation") QCOMPARE(archiveRequests, 1);
            else { const bool fresh = mode == "resume-corrupt" || mode == "resume-zero206" || mode == "resume-identity"; QCOMPARE(ranges[1], fresh ? QString() : QString::number(half)); }
            if (rejected) QCOMPARE(ranges[2], mode == "resume-truncated" ? QString::number(half + (archiveBytes.size() - half) / 2) : QString());
            if (mode == "resume-redirect") { QCOMPARE(archiveRequests, 3); QCOMPARE(ranges[2], QString::number(half)); QVERIFY(redirectRetainedBytes); QFile calls(home.path() + "/attestation-calls"); QVERIFY(calls.open(QIODevice::ReadOnly)); QCOMPARE(calls.readAll().count("/runtime.tar.zst --repo"), 1); QCOMPARE(QDir(root + "/data/runtimes").entryList(QDir::Dirs | QDir::NoDotAndDotDot), QStringList{tag}); }
            if (mode == "resume-ignored" || mode == "resume-corrupt") QVERIFY(!QDir(cache).entryList({"retained-*.part"}, QDir::Files).isEmpty());
            if (mode == "resume-identity") { QFile retained(part); QVERIFY(retained.open(QIODevice::ReadOnly)); QCOMPARE(retained.readAll(), archiveBytes.left(half)); QCOMPARE(QDir(tagCache).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 2); }
            else { QFile complete(part); QVERIFY(complete.open(QIODevice::ReadOnly)); QCOMPARE(complete.readAll(), archiveBytes); }
            QVERIFY(identityRequested); QVERIFY(QFileInfo::exists(root + "/data/runtimes/" + tag + "/usr/local/bin/darling")); return;
        }
        RuntimeDownload download(nullptr, QUrl(base)); QSignalSpy completed(&download, &RuntimeDownload::completed), progress(&download, &RuntimeDownload::progress);
        if (mode == "install-cancel") connect(&download, &RuntimeDownload::progress, &download, [&](const QString &phase) { if (phase == "Verifying and installing runtime") download.cancel(); }, Qt::DirectConnection);
        if (mode == "library-cancel") connect(&download, &RuntimeDownload::progress, &download, [&](const QString &phase) { if (phase == "Checking host libraries") QTimer::singleShot(300, &download, &RuntimeDownload::cancel); });
        download.start(root + "/data"); if (mode == "cancel" || mode == "verify-cancel") QTimer::singleShot(300, &download, &RuntimeDownload::cancel);
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 10000);
        const auto result = completed.first();
        if (mode == "success" || mode.startsWith("redirect-")) {
            QVERIFY2(result[1].toString().isEmpty(), qPrintable(result[1].toString()));
            QCOMPARE(result[0].toString(), root + "/data/runtimes/" + tag + "/usr/local"); QVERIFY(!progress.isEmpty());
            QFile calls(home.path() + "/attestation-calls"); QVERIFY(calls.open(QIODevice::ReadOnly)); const auto args = calls.readAll();
            QCOMPARE(args.split('\n').size(), 4);
            QVERIFY(args.contains("manifest.json --repo VibeDarling/darling")); QVERIFY(args.contains("SHA256SUMS --repo VibeDarling/darling")); QVERIFY(args.contains("--source-ref refs/heads/master --deny-self-hosted-runners"));
            QVERIFY(args.contains((root + "/data/runtimes/.archive-").toUtf8())); QVERIFY(args.contains("/runtime.tar.zst --repo"));
        } else { QVERIFY2(!result[1].toString().isEmpty(), qPrintable(mode)); QVERIFY(result[0].toString().isEmpty()); QVERIFY(!QFileInfo::exists(root + "/data/runtimes/" + tag)); }
        if (mode == "owner-spoof") { QVERIFY(result[1].toString().contains("escapes the runtime folder")); QVERIFY(!QFileInfo::exists(root + "/outside")); }
        if (mode == "parent-symlink") QVERIFY(result[1].toString().contains("runtime layout"));
        if (mode == "unpacked") QVERIFY(result[1].toString().contains("in manifest.json"));
        if (mode == "install-cancel") QCOMPARE(result[1].toString(), QString("Cancelled"));
        QVERIFY(identityRequested); if (mode.startsWith("redirect-")) QCOMPARE(redirects, 1);
        if (mode == "library-cap") QVERIFY(result[1].toString().contains("output limit exceeded"));
        if (mode == "library-start") QVERIFY(result[1].toString().contains("failed to start"));
        if (mode == "library-cancel") QCOMPARE(result[1].toString(), QString("Cancelled"));
    }
};
QTEST_MAIN(RuntimeTest)
#include "runtime_test.moc"
