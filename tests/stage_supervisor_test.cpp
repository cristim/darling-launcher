// SPDX-License-Identifier: GPL-3.0-or-later
#include "stagesupervisor.h"
#include <QCoreApplication>
#include <QElapsedTimer>
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
    static QProcess *supervise(const QString &script, QObject *parent) {
        auto *p = new QProcess(parent); p->setProcessChannelMode(QProcess::MergedChannels);
        auto env = QProcessEnvironment::systemEnvironment(); env.insert(StageSupervisor::ParentEnv, QString::number(QCoreApplication::applicationPid())); p->setProcessEnvironment(env);
        p->start(QCoreApplication::applicationFilePath(), {StageSupervisor::Flag, "/bin/sh", "-c", script});
        return p;
    }
private slots:
    void exitStatusIsTheAgents() {
        QScopedPointer<QProcess> p(supervise("exit 7", nullptr));
        QVERIFY(p->waitForFinished(10000)); QCOMPARE(p->exitCode(), 7);
    }
    // setsid and a double fork leave the agent's process tree; the subreaper still owns them.
    void orphansAreKilledWhenTheAgentExits() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const QString pid = dir.path() + "/pid";
        QScopedPointer<QProcess> p(supervise("setsid sh -c 'echo $$ > " + pid + "; cd /; exec sleep 600' </dev/null >/dev/null 2>&1 & sleep 1; exit 0", nullptr));
        QVERIFY(p->waitForFinished(15000)); QCOMPARE(p->exitCode(), 0);
        QVERIFY(QFile::exists(pid)); QVERIFY(!alive(pid));
        QVERIFY(p->readAll().contains("stopped 1 process(es)"));
    }
    // The launcher dies: the supervisor gets SIGTERM and takes the agent with it.
    void agentStopsWhenTheLauncherDies() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const QString pid = dir.path() + "/pid";
        const QString self = QCoreApplication::applicationFilePath();
        // a short-lived "launcher" shell starts the supervisor and exits
        QProcess launcher; launcher.start("/bin/sh", {"-c", QString("%1=$$ \"%2\" %3 /bin/sh -c 'echo $$ > %4; exec sleep 600' & sleep 1").arg(StageSupervisor::ParentEnv, self, StageSupervisor::Flag, pid)});
        QVERIFY(launcher.waitForFinished(10000));
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(pid), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!alive(pid), 10000);
    }
    void refusesToStartWithoutItsLauncher() {
        QProcess p; auto env = QProcessEnvironment::systemEnvironment(); env.insert(StageSupervisor::ParentEnv, "1"); p.setProcessEnvironment(env);
        p.start(QCoreApplication::applicationFilePath(), {StageSupervisor::Flag, "/bin/true"});
        QVERIFY(p.waitForFinished(5000)); QCOMPARE(p.exitCode(), 2);
    }
    void sigtermStopsAnAgentThatIgnoresIt() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const QString pid = dir.path() + "/pid";
        QScopedPointer<QProcess> p(supervise("trap '' TERM; sh -c 'echo $$ > " + pid + "; exec sleep 600' & wait", nullptr));
        QTRY_VERIFY_WITH_TIMEOUT(alive(pid), 5000);
        QElapsedTimer timer; timer.start(); p->terminate();
        QVERIFY(p->waitForFinished(15000)); QVERIFY(timer.elapsed() < 10000);
        QVERIFY(!alive(pid));
    }
};
int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], StageSupervisor::Flag) == 0) return StageSupervisor::run(argc, argv);
    QCoreApplication app(argc, argv);
    StageSupervisorTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "stage_supervisor_test.moc"
