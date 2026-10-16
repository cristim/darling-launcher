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
#include <signal.h>

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
        for (const auto &mode : {"success", "window", "window-cancel", "window-cancel-hung", "window-close-shutdown", "window-failure", "window-none", "none", "schema", "architecture", "checksum", "hash", "size", "oversize", "http", "cancel", "attestation", "verify-cancel", "host", "glibc", "redirect"}) QTest::newRow(mode) << QString(mode);
    }
    void download() {
        QFETCH(QString, mode);
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); const QString root = temporary.path();
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        const QString base = "http://127.0.0.1:" + QString::number(server.serverPort());
        const QString tag = "v2026.10.14-aaaaaaa", name = "darling-runtime-" + tag + "-linux-" + LauncherReleases::hostArchitecture() + ".tar.zst";
        QVERIFY(QDir().mkpath(root + "/image/usr/local/bin")); QVERIFY(QDir().mkpath(root + "/image/usr/local/libexec/darling/private/etc"));
        QVERIFY(write(root + "/image/usr/local/bin/darling", mode == "window-close-shutdown"
            ? "#!/bin/sh\nif [ \"$1\" = shutdown ]; then /usr/bin/sleep 0.5; touch \"$DPREFIX/shutdown-done\"; exit 0; fi\ntouch \"$DPREFIX/initializing\"\nwhile :; do /usr/bin/sleep 0.05; done\n"
            : mode == "window-cancel-hung"
            ? "#!/bin/sh\nif [ \"$1\" = shutdown ]; then touch \"$DPREFIX/shutdown\"; exit 0; fi\necho $$ > \"$DPREFIX/pid\"\ntouch \"$DPREFIX/initializing\"\nwhile :; do /usr/bin/sleep 0.05; done\n"
            : mode == "window-cancel"
            ? "#!/bin/sh\nif [ \"$1\" = shutdown ]; then touch \"$DPREFIX/shutdown\"; exit 0; fi\ntouch \"$DPREFIX/initializing\"\nwhile [ ! -e \"$DPREFIX/shutdown\" ]; do /usr/bin/sleep 0.05; done\nexit 0\n"
            : "#!/bin/sh\nif [ -e \"$HOME/fixture-initialize-fail\" ]; then exit 1; fi\nprintf '%s\\n' \"$DARLING_INSTALL_PREFIX\" > \"$DPREFIX/runtime-used\"\nexit 0\n", true));
        QProcess tar; tar.start("tar", {"--zstd", "-cf", root + "/fixture.tar.zst", "-C", root + "/image", "usr"}); QVERIFY(tar.waitForFinished()); QCOMPARE(tar.exitCode(), 0);
        QFile archive(root + "/fixture.tar.zst"); QVERIFY(archive.open(QIODevice::ReadOnly)); const auto archiveBytes = archive.readAll();
        const auto digest = QCryptographicHash::hash(archiveBytes, QCryptographicHash::Sha256).toHex();
        QJsonArray assets;
        for (const auto &file : QStringList{"manifest.json", "SHA256SUMS", "refs.lock.json", "nested.refs.lock.json", name}) {
            QJsonObject object{{"name", file}, {"browser_download_url", base + '/' + file}};
            if (file == name) { object["size"] = archiveBytes.size(); object["digest"] = "sha256:" + QString::fromLatin1(digest); }
            assets.append(object);
        }
        QJsonObject release{{"tag_name", tag}, {"published_at", "2026-10-14T10:00:00Z"}, {"assets", assets}};
        QJsonObject artifact{{"file", name}, {"url", base + '/' + name}, {"sha256", QString::fromLatin1(digest)}, {"size", archiveBytes.size() + (mode == "size" ? 1 : 0)}, {"unpacked_size", 4096}, {"install_root", "usr/local"}, {"launcher", "usr/local/bin/darling"}, {"host_requires", QJsonObject{{"glibc", "1.0"}, {"wrapped_sonames", mode == "host" ? QJsonArray{"lib-launcher-fixture-missing.so.999"} : QJsonArray{}}}}};
        QJsonObject manifest{{"schema", mode == "schema" ? 2 : 1}, {"version", tag}, {"artifacts", QJsonObject{{mode == "architecture" ? "absent" : LauncherReleases::hostArchitecture(), artifact}}}};
        if (mode == "glibc") { artifact["host_requires"] = QJsonObject{{"glibc", "garbage"}, {"wrapped_sonames", QJsonArray{}}}; manifest["artifacts"] = QJsonObject{{LauncherReleases::hostArchitecture(), artifact}}; }
        QMap<QString, QByteArray> responses{{"/manifest.json", QJsonDocument(manifest).toJson()}, {"/SHA256SUMS", (mode == "checksum" ? QByteArray(64, '0') : digest) + "  " + name.toUtf8() + '\n'}, {'/' + name, archiveBytes}};
        connect(&server, &QTcpServer::newConnection, this, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                    const auto request = socket->readAll(); const auto path = QString::fromLatin1(request.split(' ').value(1));
                    if (mode == "cancel") return;
                    QByteArray body;
                    if (path.startsWith("/repos/")) body = QJsonDocument(mode.endsWith("none") ? QJsonArray{QJsonObject{{"tag_name", "v0.1.20260921"}}} : QJsonArray{release}).toJson();
                    else body = responses.value(path);
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
            const QString closedMarker = windowHome + "/.darling-launcher/prefixes/prebuilt/shutdown-done";
            auto shutdownSurvivesClose = qScopeGuard([&] { if (mode == "window-close-shutdown") QVERIFY2(QFileInfo::exists(closedMarker), "shutdown was killed when the window was destroyed"); });
            Window window({}, false, [] { return QList<SourceMount>{}; }, QUrl(base)); window.show();
            window.findChild<QLineEdit *>("darlingField")->clear(); window.findChild<QLineEdit *>("runtimeRootField")->clear();
            window.offerRuntimeSetup(); QVERIFY(window.findChild<QDialog *>("runtimeDownloadDialog"));
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
            if (mode == "window-close-shutdown") {
                QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(windowHome + "/.darling-launcher/prefixes/prebuilt/initializing"), 10000);
                window.findChild<QPushButton *>("runtimeDownloadCancel")->click();
                QTest::qWait(100);
                return;
            }
            if (mode == "window-cancel-hung") {
                const QString newPrefix = windowHome + "/.darling-launcher/prefixes/prebuilt";
                QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(newPrefix + "/initializing"), 10000);
                QFile pidFile(newPrefix + "/pid"); QVERIFY(pidFile.open(QIODevice::ReadOnly)); const pid_t pid = pidFile.readAll().trimmed().toInt(); QVERIFY(pid > 1);
                window.findChild<QPushButton *>("runtimeDownloadCancel")->click();
                QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(newPrefix + "/shutdown"), 5000);
                QTRY_VERIFY_WITH_TIMEOUT(::kill(pid, 0) != 0, 10000);
                return;
            }
            if (mode == "window-cancel") {
                const QString newPrefix = windowHome + "/.darling-launcher/prefixes/prebuilt";
                QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(newPrefix + "/initializing"), 10000);
                window.findChild<QPushButton *>("runtimeDownloadCancel")->click();
                QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(newPrefix + "/shutdown"), 5000);
                QTRY_VERIFY_WITH_TIMEOUT(!window.findChild<QDialog *>("runtimeDownloadDialog")->property("runtimeInitializationPending").toBool(), 5000);
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
        RuntimeDownload download(nullptr, QUrl(base)); QSignalSpy completed(&download, &RuntimeDownload::completed), progress(&download, &RuntimeDownload::progress);
        download.start(root + "/data"); if (mode == "cancel" || mode == "verify-cancel") QTimer::singleShot(300, &download, &RuntimeDownload::cancel);
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 10000);
        const auto result = completed.first();
        if (mode == "success") {
            QVERIFY2(result[1].toString().isEmpty(), qPrintable(result[1].toString()));
            QCOMPARE(result[0].toString(), root + "/data/runtimes/" + tag + "/usr/local"); QVERIFY(!progress.isEmpty());
            QFile calls(home.path() + "/attestation-calls"); QVERIFY(calls.open(QIODevice::ReadOnly)); const auto args = calls.readAll();
            QCOMPARE(args.split('\n').size(), 4);
            QVERIFY(args.contains("manifest.json --repo VibeDarling/darling")); QVERIFY(args.contains("SHA256SUMS --repo VibeDarling/darling")); QVERIFY(args.contains("--source-ref refs/heads/master --deny-self-hosted-runners"));
        } else { QVERIFY2(!result[1].toString().isEmpty(), qPrintable(mode)); QVERIFY(result[0].toString().isEmpty()); QVERIFY(!QFileInfo::exists(root + "/data/runtimes/" + tag)); }
    }
};
QTEST_MAIN(RuntimeTest)
#include "runtime_test.moc"
