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
    void offStripsInheritedVariables() {
        QTemporaryDir prefix; QVERIFY(prefix.isValid());
        QProcessEnvironment env; env.insert("KEEP", "1"); env.insert(TraceVars::Mode, "trap"); env.insert(TraceVars::Log, "/x"); env.insert(QString(TraceVars::Mode) + "_FUTURE", "1");
        QString error; QVERIFY(LauncherTrace::applyTrace(env, prefix.path(), "App", false, "log", nullptr, &error));
        QCOMPARE(env.keys(), QStringList{"KEEP"});
        env.insert(TraceVars::Log, "/elsewhere"); QVERIFY(LauncherTrace::applyTrace(env, prefix.path(), "App", true, "log", nullptr, &error));
        QVERIFY(env.value(TraceVars::Log).startsWith(LauncherTrace::traceDirectory(prefix.path())));
    }
    void countersSaturateAndLongLinesAreSkipped() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); QByteArray data;
        for (int i = 0; i < 2000; ++i) data += "{\"image\":\"/F\",\"symbol\":\"_a\",\"count\":900000000}\n";
        data += "{\"image\":\"/F\",\"symbol\":\"_b\",\"count\":1e300}\n{\"symbol\":\"" + QByteArray(100000, 'x') + "\"}\n";
        const auto summary = LauncherTrace::parse(write(dir.path() + "/c.jsonl", data));
        QCOMPARE(summary.skipped, 1); QCOMPARE(summary.rows.size(), 2);
        QCOMPARE(summary.rows[0].count, LauncherTrace::MaxCount);
        QCOMPARE(summary.rows[1].count, 1);
    }
    void rowCountIsCapped() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); QByteArray data; for (int i = 0; i < LauncherTrace::MaxRows + 50; ++i) data += "{\"symbol\":\"s" + QByteArray::number(i) + "\"}\n";
        const auto summary = LauncherTrace::parse(write(dir.path() + "/r.jsonl", data));
        QCOMPARE(summary.rows.size(), LauncherTrace::MaxRows); QVERIFY(summary.truncated); QCOMPARE(summary.skipped, 50);
    }
    void hostileTextCannotForgeOutput() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto summary = LauncherTrace::parse(write(dir.path() + "/h.jsonl", "{\"image\":\"/F\\t9\\n/Forged\",\"symbol\":\"_a\\u001b[31m\\nforged\\t1\\r\\u202e\",\"count\":1}\n{\"symbol\":\"" + QByteArray(5000, 'y') + "\"}\n"));
        QCOMPARE(summary.rows.size(), 2);
        for (const auto &row : summary.rows) for (const QString &text : {row.image, row.symbol}) { QVERIFY(text.size() <= LauncherTrace::MaxText); for (QChar c : text) QVERIFY(c.isPrint() && c != '\t' && c != '\n' && c != QChar(0x202e)); }
        const QString exported = LauncherTrace::exportSummary(summary);
        QCOMPARE(exported.count('\n'), 3); for (const QString &line : exported.split('\n', Qt::SkipEmptyParts)) QCOMPARE(line.count('\t'), 2);
    }
    void symlinkedLogIsNotFollowedAndSummaryIsPrivate() {
        QTemporaryDir prefix, other; QVERIFY(prefix.isValid() && other.isValid());
        const QString target = write(other.path() + "/real.jsonl", "{\"symbol\":\"_a\"}\n");
        QVERIFY(QFile::link(target, other.path() + "/link.jsonl"));
        QVERIFY(LauncherTrace::parse(other.path() + "/link.jsonl").unreadable);
        QString path, error; LauncherTrace::Summary summary; summary.rows.append({"/F", "_a", {}, 1});
        QVERIFY2(LauncherTrace::writeSummary(prefix.path(), summary, &path, &error), qPrintable(error));
        QCOMPARE(QFileInfo(path).permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther), QFileDevice::Permissions());
        QVERIFY(!LauncherTrace::writeSummary(prefix.path(), summary, &path, &error) || QFileInfo(path).size() > 0);
        QTemporaryDir swapped; QVERIFY(QDir().mkpath(swapped.path() + "/.darling-launcher")); QVERIFY(QFile::link(other.path(), LauncherTrace::traceDirectory(swapped.path())));
        QVERIFY(!LauncherTrace::writeSummary(swapped.path(), summary, &path, &error)); QVERIFY(!QFileInfo::exists(other.path() + "/" + QFileInfo(path).fileName()));
    }
    void parserCapsBytesRead() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); QByteArray data; for (int i = 0; i < 1000; ++i) data += "{\"image\":\"/F\",\"symbol\":\"_s" + QByteArray::number(i) + "\"}\n";
        const auto summary = LauncherTrace::parse(write(dir.path() + "/big.jsonl", data), 4096);
        QVERIFY(summary.truncated); QVERIFY(summary.rows.size() > 10 && summary.rows.size() < 1000); QCOMPARE(summary.skipped, 0);
    }
};
QTEST_GUILESS_MAIN(TraceTest)
#include "trace_test.moc"
