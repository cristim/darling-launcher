// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtimedownload.h"
#include "releases.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

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
        for (const auto &mode : {"success", "none", "schema", "architecture", "checksum", "hash", "size", "oversize", "http", "cancel", "attestation", "verify-cancel", "host", "glibc", "redirect", "owner-spoof", "parent-symlink", "unpacked", "install-cancel", "redirect-valid", "redirect-relative", "redirect-stream", "library-cap", "library-start", "library-cancel"}) QTest::newRow(mode) << QString(mode);
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
        QVERIFY(write(root + "/image/usr/local/bin/darling", "#!/bin/sh\nif [ -e \"$HOME/fixture-initialize-fail\" ]; then exit 1; fi\nprintf '%s\\n' \"$DARLING_INSTALL_PREFIX\" > \"$DPREFIX/runtime-used\"\nexit 0\n", true));
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
        bool identityRequested = true; int redirects = 0;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                    const auto request = socket->readAll(); const auto path = QString::fromLatin1(request.split(' ').value(1));
                    identityRequested &= request.contains("Accept-Encoding: identity\r\n");
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
