// SPDX-License-Identifier: GPL-3.0-or-later
#include "stagesupervisor.h"
#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <cstring>
#include <signal.h>
class StageSupervisorTest : public QObject {
    Q_OBJECT
    static bool alive(const QString &pidFile) {
        QFile f(pidFile); if (!f.open(QIODevice::ReadOnly)) return false;
        const qint64 pid = f.readAll().trimmed().toLongLong();
        return pid > 0 && ::kill(pid_t(pid), 0) == 0;
    }
private slots:
    void exitStatusIsTheAgents() {
        QProcess p; p.start(QCoreApplication::applicationFilePath(), {StageSupervisor::Flag, "/bin/sh", "-c", "exit 7"});
        QVERIFY(p.waitForFinished(10000)); QCOMPARE(p.exitCode(), 7);
    }
    // setsid and a double fork leave the agent's process tree; the subreaper still owns them.
    void orphansAreKilledWhenTheAgentExits() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const QString pid = dir.path() + "/pid";
        QProcess p; p.start(QCoreApplication::applicationFilePath(), {StageSupervisor::Flag, "/bin/sh", "-c", "setsid sh -c 'echo $$ > " + pid + "; cd /; exec sleep 600' </dev/null >/dev/null 2>&1 & sleep 1; exit 0"});
        QVERIFY(p.waitForFinished(15000)); QCOMPARE(p.exitCode(), 0);
        QVERIFY(QFile::exists(pid)); QTRY_VERIFY_WITH_TIMEOUT(!alive(pid), 5000);
    }
};
int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], StageSupervisor::Flag) == 0) return StageSupervisor::run(argc, argv);
    QCoreApplication app(argc, argv);
    StageSupervisorTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "stage_supervisor_test.moc"
