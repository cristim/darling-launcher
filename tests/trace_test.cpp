// SPDX-License-Identifier: GPL-3.0-or-later
#include "trace.h"
#include "tracevars.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <sys/stat.h>
#include <unistd.h>

class TraceTest : public QObject {
    Q_OBJECT
    QTemporaryDir tools;
    QString plain() { return fake(0755); }
    QString fake(int mode, const QString &name = "darling") { const QString path = tools.path() + "/" + name + QString::number(mode, 8); if (!QFileInfo::exists(path)) { write(path, "#!/bin/sh\n"); QFile::setPermissions(path, QFileDevice::Permissions::fromInt(mode & 0777)); chmod(QFile::encodeName(path).constData(), mode); } return path; }
    static QString write(const QString &path, const QByteArray &data) { QFile f(path); if (f.open(QIODevice::WriteOnly)) f.write(data); return path; }
private slots:
    void offLeavesEnvironmentIdentical() {
        QTemporaryDir prefix; QVERIFY(prefix.isValid());
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment(); env.insert("DPREFIX", prefix.path()); const auto before = env.toStringList();
        QString error; QVERIFY(LauncherTrace::applyTrace(env, prefix.path(), false, plain(), &error));
        QCOMPARE(env.toStringList(), before); QVERIFY(!QFileInfo::exists(prefix.path() + "/.darling-launcher"));
    }
    void onSetsBothVariablesPointingAtPrivateDirectory() {
        QTemporaryDir prefix; QVERIFY(prefix.isValid());
        QProcessEnvironment env; QString error;
        QVERIFY2(LauncherTrace::applyTrace(env, prefix.path(), true, plain(), &error), qPrintable(error));
        QCOMPARE(env.value(TraceVars::Mode), QString("log")); QCOMPARE(env.value(TraceVars::Log), QString("/.darling-launcher/trace"));
        const QString dir = LauncherTrace::traceDirectory(prefix.path());
        QVERIFY(QFileInfo(dir).isDir()); QVERIFY(QDir(dir).isEmpty());
        QCOMPARE(QFileInfo(dir).permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther), QFileDevice::Permissions());
    }
    void refusesSymlinkedOrExposedDirectory() {
        QTemporaryDir prefix, other; QVERIFY(prefix.isValid() && other.isValid());
        QVERIFY(QDir().mkpath(prefix.path() + "/.darling-launcher"));
        QVERIFY(QFile::link(other.path(), LauncherTrace::traceDirectory(prefix.path())));
        QProcessEnvironment env; QString error;
        QVERIFY(!LauncherTrace::applyTrace(env, prefix.path(), true, plain(), &error)); QVERIFY(env.isEmpty()); QVERIFY(QDir(other.path()).isEmpty());
        QTemporaryDir open; QVERIFY(QDir().mkpath(open.path() + "/.darling-launcher/trace"));
        QVERIFY(QFile::setPermissions(open.path() + "/.darling-launcher/trace", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadOther | QFile::ExeOther));
        QVERIFY(!LauncherTrace::applyTrace(env, open.path(), true, plain(), &error)); QVERIFY(error.contains("other users"));
    }
    void refusesForeignOwnedDirectory() {
        if (::geteuid() == 0) QSKIP("root owns everything it can create");
        QString error; QVERIFY(!LauncherTrace::ensurePrivateDirectory("/usr", &error)); QVERIFY2(error.contains("not owned"), qPrintable(error));
    }
    void parserToleratesGarbageAndSkipsMalformedRecords() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString path = write(dir.path() + "/t.jsonl",
            "{\"v\":1,\"kind\":\"missing\",\"sym\":\"_a\",\"lib\":\"/F\",\"from\":\"/App/A\",\"arch\":\"arm64\",\"unknown\":[1]}\n"
            "not json\n{\"v\":1,\"kind\":\"missing\"}\n{\"v\":1,\"kind\":\"mystery\",\"sym\":\"_x\"}\n\n[1,2]\n"
            "{\"v\":1,\"kind\":\"missing\",\"sym\":\"_b\",\"lib\":\"/G\",\"from\":\"/App/B\"}\n\xff\xfe\n");
        const auto summary = LauncherTrace::parse(path);
        QCOMPARE(summary.skipped, 5); QVERIFY(!summary.truncated); QCOMPARE(summary.rows.size(), 2);
        QCOMPARE(summary.rows[0].symbol, QString("_a")); QCOMPARE(summary.rows[0].library, QString("/F")); QCOMPARE(summary.rows[0].from, QString("/App/A")); QCOMPARE(summary.rows[0].kind, QString("missing"));
        QCOMPARE(summary.rows[1].symbol, QString("_b")); QCOMPARE(summary.rows[1].library, QString("/G"));
        QCOMPARE(summary.rows[1].kind, QString("missing"));
        QVERIFY(!LauncherTrace::exportSummary(summary).contains("unknown"));
        QVERIFY(LauncherTrace::exportSummary(summary).startsWith("symbol\tlibrary\treferencing_image\tkind\n_a\t/F\t/App/A\tmissing\n"));
    }
    void malformedVersionsAndFieldTypesAreSkipped() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QList<QByteArray> records{
            "{\"kind\":\"missing\",\"sym\":\"_f\"}",
            "{\"v\":2,\"kind\":\"missing\",\"sym\":\"_f\"}",
            "{\"v\":true,\"kind\":\"missing\",\"sym\":\"_f\"}",
            "{\"v\":\"1\",\"kind\":\"missing\",\"sym\":\"_f\"}",
            "{\"v\":1.5,\"kind\":\"missing\",\"sym\":\"_f\"}",
            "{\"v\":1,\"kind\":\"missing\",\"sym\":\"_f\",\"lib\":[]}",
            "{\"v\":1,\"kind\":\"missing\",\"sym\":\"_f\",\"from\":42}",
            "{\"v\":1,\"kind\":\"truncated\",\"arch\":false}",
            "{\"v\":1,\"kind\":\"scan_skipped\",\"reason\":{}}",
            "{\"v\":1,\"kind\":\"truncated\",\"sym\":null}"
        };
        for (const auto &record : records) {
            const auto summary = LauncherTrace::parse(write(dir.path() + "/malformed.jsonl", record + '\n'));
            QCOMPARE(summary.rows.size(), 0);
            QCOMPARE(summary.skipped, 1);
        }
    }
    void truncatedAndScanSkippedRecordsAreSurfaced() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto summary = LauncherTrace::parse(write(dir.path() + "/e.jsonl",
            "{\"v\":1,\"kind\":\"missing\",\"sym\":\"_a\",\"lib\":\"/l\",\"from\":\"/f\"}\n"
            "{\"v\":1,\"kind\":\"truncated\"}\n"
            "{\"v\":1,\"kind\":\"truncated\",\"lib\":\"/l\",\"from\":\"/f\",\"arch\":\"arm64\"}\n"
            "{\"v\":1,\"kind\":\"scan_skipped\",\"reason\":\"scan_budget\"}\n"));
        QCOMPARE(summary.skipped, 0); QCOMPARE(summary.rows.size(), 4);
        QCOMPARE(summary.rows[0].kind, QString("missing"));
        QCOMPARE(summary.rows[1].kind, QString("truncated")); QVERIFY(summary.rows[1].symbol.isEmpty());
        QCOMPARE(summary.rows[2].library, QString("/l"));
        QCOMPARE(summary.rows[3].kind, QString("scan_skipped")); QCOMPARE(summary.rows[3].reason, QString("scan_budget"));
        QVERIFY(LauncherTrace::exportSummary(summary).contains("scan_budget"));
    }
    void elevatedDarlingNeverGetsVariables() {
        QTemporaryDir prefix; QVERIFY(prefix.isValid()); QString error;
        for (const QString &darling : {fake(04755), fake(02755)}) {
            QProcessEnvironment env; env.insert(TraceVars::Log, "/x");
            QVERIFY(LauncherTrace::elevated(darling)); QVERIFY(LauncherTrace::applyTrace(env, prefix.path(), true, darling, &error)); QVERIFY(env.isEmpty());
        }
        QVERIFY(!QFileInfo::exists(LauncherTrace::traceDirectory(prefix.path())));
        QVERIFY(!LauncherTrace::elevated(plain())); QVERIFY(LauncherTrace::elevated(plain(), 0)); QVERIFY(LauncherTrace::elevated(tools.path() + "/missing"));
        QProcessEnvironment env; QVERIFY(LauncherTrace::applyTrace(env, prefix.path(), true, plain(), &error)); QVERIFY(env.contains(TraceVars::Log));
        QVERIFY(QFileInfo(LauncherTrace::traceDirectory(prefix.path())).isDir()); QVERIFY(QDir(LauncherTrace::traceDirectory(prefix.path())).isEmpty());
    }
    void offStripsInheritedVariables() {
        QTemporaryDir prefix; QVERIFY(prefix.isValid());
        QProcessEnvironment env; env.insert("KEEP", "1"); env.insert(TraceVars::Mode, "trap"); env.insert(TraceVars::Log, "/x"); env.insert(QString(TraceVars::Mode) + "_FUTURE", "1");
        QString error; QVERIFY(LauncherTrace::applyTrace(env, prefix.path(), false, plain(), &error));
        QCOMPARE(env.keys(), QStringList{"KEEP"});
        env.insert(TraceVars::Log, "/elsewhere"); QVERIFY(LauncherTrace::applyTrace(env, prefix.path(), true, plain(), &error));
        QCOMPARE(env.value(TraceVars::Log), QString("/.darling-launcher/trace"));
    }
    void longLinesAndUnknownKindsAreSkipped() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QByteArray data = "{\"v\":1,\"kind\":\"missing\",\"sym\":\"" + QByteArray(100000, 'x') + "\",\"lib\":\"/l\",\"from\":\"/f\"}\n";
        data += "{\"v\":1,\"kind\":\"missing\",\"sym\":\"_a\",\"lib\":\"/F\",\"from\":\"/App\"}\n";
        const auto summary = LauncherTrace::parse(write(dir.path() + "/c.jsonl", data));
        QCOMPARE(summary.skipped, 1); QCOMPARE(summary.rows.size(), 1);
        QCOMPARE(summary.rows[0].symbol, QString("_a"));
    }
    void rowCountIsCapped() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); QByteArray data; for (int i = 0; i < LauncherTrace::MaxRows + 50; ++i) data += "{\"v\":1,\"kind\":\"missing\",\"sym\":\"s" + QByteArray::number(i) + "\",\"lib\":\"/l\",\"from\":\"/f\"}\n";
        const auto summary = LauncherTrace::parse(write(dir.path() + "/r.jsonl", data));
        QCOMPARE(summary.rows.size(), LauncherTrace::MaxRows); QVERIFY(summary.truncated); QCOMPARE(summary.skipped, 50);
    }
    void hostileTextCannotForgeOutput() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto summary = LauncherTrace::parse(write(dir.path() + "/h.jsonl",
            "{\"v\":1,\"kind\":\"missing\",\"sym\":\"_a\\u001b[31m\\nforged\\t1\\r\\u202e\",\"lib\":\"/F\\t9\\n/Forged\",\"from\":\"/App\\u0000x\"}\n"
            "{\"v\":1,\"kind\":\"missing\",\"sym\":\"" + QByteArray(5000, 'y') + "\",\"lib\":\"/l\",\"from\":\"/f\"}\n"));
        QCOMPARE(summary.rows.size(), 2);
        for (const auto &row : summary.rows) for (const QString &text : {row.symbol, row.library, row.from, row.kind}) { QVERIFY(text.size() <= LauncherTrace::MaxText); for (QChar c : text) QVERIFY(c.isPrint() && c != '\t' && c != '\n' && c != QChar(0x202e)); }
        const QString exported = LauncherTrace::exportSummary(summary);
        QCOMPARE(exported.count('\n'), 3); for (const QString &line : exported.split('\n', Qt::SkipEmptyParts)) QCOMPARE(line.count('\t'), 3);
    }
    void symlinkedLogIsNotFollowedAndSummaryIsPrivate() {
        QTemporaryDir prefix, other; QVERIFY(prefix.isValid() && other.isValid());
        const QString target = write(other.path() + "/real.jsonl", "{\"v\":1,\"kind\":\"missing\",\"sym\":\"_a\",\"lib\":\"/l\",\"from\":\"/f\"}\n");
        QVERIFY(QFile::link(target, other.path() + "/link.jsonl"));
        QVERIFY(LauncherTrace::parse(other.path() + "/link.jsonl").unreadable);
        QString path, error; LauncherTrace::Summary summary; LauncherTrace::Row row; row.symbol = "_a"; row.library = "/l"; row.from = "/f"; row.kind = "missing"; summary.rows.append(row);
        QVERIFY2(LauncherTrace::writeSummary(prefix.path(), summary, &path, &error), qPrintable(error));
        QCOMPARE(QFileInfo(path).permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther), QFileDevice::Permissions());
        QVERIFY(!LauncherTrace::writeSummary(prefix.path(), summary, &path, &error) || QFileInfo(path).size() > 0);
        QTemporaryDir swapped; QVERIFY(QDir().mkpath(swapped.path() + "/.darling-launcher")); QVERIFY(QFile::link(other.path(), LauncherTrace::traceDirectory(swapped.path())));
        QVERIFY(!LauncherTrace::writeSummary(swapped.path(), summary, &path, &error)); QVERIFY(!QFileInfo::exists(other.path() + "/" + QFileInfo(path).fileName()));
    }
    void parserCapsBytesRead() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); QByteArray data; for (int i = 0; i < 1000; ++i) data += "{\"v\":1,\"kind\":\"missing\",\"sym\":\"_s" + QByteArray::number(i) + "\",\"lib\":\"/l\",\"from\":\"/f\"}\n";
        const auto summary = LauncherTrace::parse(write(dir.path() + "/big.jsonl", data), 4096);
        QVERIFY(summary.truncated); QVERIFY(summary.rows.size() > 10 && summary.rows.size() < 1000); QCOMPARE(summary.skipped, 0);
    }
};
QTEST_GUILESS_MAIN(TraceTest)
#include "trace_test.moc"
