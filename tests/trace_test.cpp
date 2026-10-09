// SPDX-License-Identifier: GPL-3.0-or-later
#include "trace.h"
#include "tracevars.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <unistd.h>

class TraceTest : public QObject {
    Q_OBJECT
    static QString write(const QString &path, const QByteArray &data) { QFile f(path); if (f.open(QIODevice::WriteOnly)) f.write(data); return path; }
private slots:
    void offLeavesEnvironmentIdentical() {
        QTemporaryDir prefix; QVERIFY(prefix.isValid());
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment(); env.insert("DPREFIX", prefix.path()); const auto before = env.toStringList();
        QString error; QVERIFY(LauncherTrace::applyTrace(env, prefix.path(), "App", false, "log", nullptr, &error));
        QCOMPARE(env.toStringList(), before); QVERIFY(!QFileInfo::exists(prefix.path() + "/.darling-launcher"));
    }
    void onSetsBothVariablesUnderPrivateDirectory() {
        QTemporaryDir prefix; QVERIFY(prefix.isValid());
        QProcessEnvironment env; QString path, error;
        QVERIFY2(LauncherTrace::applyTrace(env, prefix.path(), "My App/../x", true, "trap", &path, &error), qPrintable(error));
        QCOMPARE(env.value(TraceVars::Mode), QString("trap")); QCOMPARE(env.value(TraceVars::Log), path);
        QVERIFY(path.startsWith(LauncherTrace::traceDirectory(prefix.path()) + "/My_App_.._x-")); QVERIFY(path.endsWith("Z.jsonl"));
        QCOMPARE(QFileInfo(LauncherTrace::traceDirectory(prefix.path())).permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther), QFileDevice::Permissions());
        QCOMPARE(QFileInfo(path).permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther | QFile::ExeOwner), QFileDevice::Permissions()); QVERIFY(QFileInfo(path).permissions() & QFile::WriteOwner);
        QVERIFY(!LauncherTrace::applyTrace(env, prefix.path(), "App", true, "bogus", nullptr, &error));
    }
    void refusesSymlinkedOrExposedDirectory() {
        QTemporaryDir prefix, other; QVERIFY(prefix.isValid() && other.isValid());
        QVERIFY(QDir().mkpath(prefix.path() + "/.darling-launcher"));
        QVERIFY(QFile::link(other.path(), LauncherTrace::traceDirectory(prefix.path())));
        QProcessEnvironment env; QString error;
        QVERIFY(!LauncherTrace::applyTrace(env, prefix.path(), "App", true, "log", nullptr, &error)); QVERIFY(env.isEmpty()); QVERIFY(QDir(other.path()).isEmpty());
        QTemporaryDir open; QVERIFY(QDir().mkpath(open.path() + "/.darling-launcher/trace"));
        QVERIFY(QFile::setPermissions(open.path() + "/.darling-launcher/trace", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadOther | QFile::ExeOther));
        QVERIFY(!LauncherTrace::applyTrace(env, open.path(), "App", true, "log", nullptr, &error)); QVERIFY(error.contains("other users"));
    }
    void refusesForeignOwnedDirectory() {
        if (::geteuid() == 0) QSKIP("root owns everything it can create");
        QString error; QVERIFY(!LauncherTrace::ensurePrivateDirectory("/usr", &error)); QVERIFY2(error.contains("not owned"), qPrintable(error));
    }
    void parserToleratesGarbageAndAggregates() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString path = write(dir.path() + "/t.jsonl",
            "{\"image\":\"/F\",\"symbol\":\"_a\",\"count\":2,\"regs\":[\"0x10\",255,\"rm -rf\",{\"x\":1}],\"unknown\":[1]}\n"
            "not json\n{\"image\":\"/F\"}\n\n[1,2]\n{\"image\":\"/F\",\"symbol\":\"_a\"}\n{\"image\":\"/G\",\"symbol\":\"_b\",\"count\":-5}\n\xff\xfe\n");
        const auto summary = LauncherTrace::parse(path);
        QCOMPARE(summary.skipped, 4); QVERIFY(!summary.truncated); QCOMPARE(summary.rows.size(), 2);
        QCOMPARE(summary.rows[0].symbol, QString("_a")); QCOMPARE(summary.rows[0].count, 3); QCOMPARE(summary.rows[0].details, QString("0x10 0xff"));
        QCOMPARE(summary.rows[1].count, 1);
        QVERIFY(!LauncherTrace::exportSummary(summary).contains("0x10"));
    }
    void parserCapsBytesRead() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); QByteArray data; for (int i = 0; i < 1000; ++i) data += "{\"image\":\"/F\",\"symbol\":\"_s" + QByteArray::number(i) + "\"}\n";
        const auto summary = LauncherTrace::parse(write(dir.path() + "/big.jsonl", data), 4096);
        QVERIFY(summary.truncated); QVERIFY(summary.rows.size() > 10 && summary.rows.size() < 1000); QCOMPARE(summary.skipped, 0);
    }
};
QTEST_GUILESS_MAIN(TraceTest)
#include "trace_test.moc"
