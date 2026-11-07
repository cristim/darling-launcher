// SPDX-License-Identifier: GPL-3.0-or-later
#include "prefixstate.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using PrefixState::ProcFs;

static void write(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(data);
}

class Fixture {
public:
    QTemporaryDir prefix, proc;
    bool live = false;
    ProcFs fs() { ProcFs p; p.procRoot = proc.path(); p.socketMayBeLive = [this](const QString &) { return live; }; return p; }
    void pidFile(const QByteArray &c) { write(prefix.path() + "/.init.pid", c); }
    void process(int pid, const QString &exeTarget) {
        QDir().mkpath(proc.path() + "/" + QString::number(pid));
        if (!exeTarget.isEmpty()) QVERIFY(QFile::link(exeTarget, proc.path() + "/" + QString::number(pid) + "/exe"));
    }
    bool running(QString *why = nullptr) { return PrefixState::prefixRunning(prefix.path(), fs(), why); }
};

class PrefixStateTest : public QObject {
    Q_OBJECT
private slots:
    void noPidNoSocketIsStopped() { Fixture f; QVERIFY(!f.running()); }
    void pidFileWithAbsentProcessIsStopped() { Fixture f; f.pidFile("4242\n"); QVERIFY(!f.running()); }
    void liveSocketMeansRunningEvenWithoutPidFile() { Fixture f; f.live = true; QVERIFY(f.running()); }
    void liveSocketMeansRunningWithStalePidFile() { Fixture f; f.live = true; f.pidFile("4242"); QVERIFY(f.running()); }
    void darlingserverProcessIsRunning() {
        Fixture f; f.pidFile("4242"); f.process(4242, "/usr/libexec/darling/bin/darlingserver");
        QString why; QVERIFY(f.running(&why)); QVERIFY(why.contains("darlingserver is running"));
    }
    void otherProcessWithPidFailsClosed() {
        Fixture f; f.pidFile("4242"); f.process(4242, "/usr/bin/bash"); QVERIFY(f.running());
    }
    void unreadableExeFailsClosed() { Fixture f; f.pidFile("4242"); f.process(4242, ""); QVERIFY(f.running()); }
    void unparsablePidFailsClosed_data() {
        QTest::addColumn<QByteArray>("content");
        QTest::newRow("text") << QByteArray("abc");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("zero") << QByteArray("0");
        QTest::newRow("negative") << QByteArray("-5");
        QTest::newRow("oversized") << QByteArray(100, '1');
        QTest::newRow("padded past the cap") << QByteArray("4242") + QByteArray(60, ' ');
    }
    void unparsablePidFailsClosed() { QFETCH(QByteArray, content); Fixture f; f.pidFile(content); QVERIFY(f.running()); }
    void symlinkedPidFileFailsClosed() {
        Fixture f; write(f.prefix.path() + "/real", "4242");
        QVERIFY(QFile::link(f.prefix.path() + "/real", f.prefix.path() + "/.init.pid")); QVERIFY(f.running());
    }
    void unreadableProcEntryFailsClosed() {
        if (geteuid() == 0) QSKIP("root bypasses directory permissions");
        Fixture f; f.pidFile("4242"); QVERIFY(QFile::setPermissions(f.proc.path(), {}));
        const bool r = f.running();
        QFile::setPermissions(f.proc.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QVERIFY(r);
    }
    void realSocketProbe() {
        QTemporaryDir dir; const QString path = dir.path() + "/s.sock";
        QVERIFY(!PrefixState::socketMayBeLive(path)); // absent
        const QByteArray p = QFile::encodeName(path);
        sockaddr_un a{}; a.sun_family = AF_UNIX; memcpy(a.sun_path, p.constData(), p.size());
        const int s = ::socket(AF_UNIX, SOCK_STREAM, 0);
        QVERIFY(::bind(s, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0 && ::listen(s, 1) == 0);
        QVERIFY(PrefixState::socketMayBeLive(path));
        ::close(s);
        QVERIFY(!PrefixState::socketMayBeLive(path)); // stale socket file: ECONNREFUSED
        QVERIFY(PrefixState::socketMayBeLive(dir.path() + "/" + QString(200, 'x'))); // unprobeable: doubt
    }
};
QTEST_APPLESS_MAIN(PrefixStateTest)
#include "prefixstate_test.moc"
