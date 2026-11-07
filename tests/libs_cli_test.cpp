// SPDX-License-Identifier: GPL-3.0-or-later
#include "libscli.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <sys/stat.h>
#include <unistd.h>

static const QString kLib = "/usr/lib/libfoo.dylib";
static const QString kAppKit = "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit";

struct Result { int code; QString out, err; };
static Result run(const QStringList &args) {
    QString o, e; QTextStream out(&o), err(&e);
    const int code = libsMain(args, out, err);
    return {code, o, e};
}
static void put(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(data);
}
static QByteArray slurp(const QString &path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }

struct Env {
    QTemporaryDir prefix, runtime, files;
    QString image() { const QString p = files.path() + "/image.bin"; put(p, "image-bytes"); return p; }
    QStringList base(const QString &verb) { return {verb, "--prefix", prefix.path()}; }
    Result import(const QString &name = kLib, QStringList extra = {}) {
        return run(QStringList{"import", "--prefix", prefix.path(), "--runtime-root", runtime.path(), "--name", name, "--file", image()} + extra);
    }
    Result list() { return run(base("list")); }
    QString upper(const QString &n = kLib) const { return prefix.path() + n; }
};

class LibsCliTest : public QObject {
    Q_OBJECT
private slots:
    void importThenListPrintsTsv() {
        Env e; const Result r = e.import(kLib, {"--source-kind", "fixture"});
        QCOMPARE(r.code, 0); QCOMPARE(r.out, QString("ok\n")); QCOMPARE(slurp(e.upper()), QByteArray("image-bytes"));
        QCOMPARE(e.list().out, kLib + "\timported\tfixture\n");
        QCOMPARE(e.import().code, 0); // idempotent
    }
    void refusedImportsExitOneWithOneLineOnStderr_data() {
        QTest::addColumn<QString>("name"); QTest::addColumn<QString>("needle");
        QTest::newRow("pinned") << kAppKit << "Darling ships";
        QTest::newRow("bad name") << "@rpath/X" << "absolute";
    }
    void refusedImportsExitOneWithOneLineOnStderr() {
        QFETCH(QString, name); QFETCH(QString, needle);
        Env e; const Result r = e.import(name);
        QCOMPARE(r.code, 1); QVERIFY2(r.err.startsWith("error: ") && r.err.contains(needle), qPrintable(r.err));
        QCOMPARE(r.err.count('\n'), 1); QVERIFY(r.out.isEmpty());
        QVERIFY(!QFileInfo::exists(e.upper(name)));
    }
    void runtimeProvidedPathIsRefusedUnlessForced() {
        Env e; put(e.runtime.path() + kLib, "runtime");
        QCOMPARE(e.import().code, 1); QVERIFY(!QFileInfo::exists(e.upper()));
        QCOMPARE(e.import(kLib, {"--force"}).code, 0); QVERIFY(QFileInfo::exists(e.upper()));
    }
    void importRefusesMissingFileOversizedKindAndRunningPrefix() {
        Env e;
        QCOMPARE(run({"import", "--prefix", e.prefix.path(), "--runtime-root", e.runtime.path(), "--name", kLib, "--file", e.files.path() + "/nope"}).code, 1);
        QCOMPARE(run({"import", "--prefix", e.prefix.path(), "--runtime-root", e.runtime.path(), "--name", kLib, "--file", e.files.path()}).code, 1);
        put(e.prefix.path() + "/.init.pid", QByteArray::number(qint64(::getpid()))); // a live process that is not darlingserver: fail closed
        const Result r = e.import(); QCOMPARE(r.code, 1); QVERIFY(r.err.contains("running"));
        QVERIFY(!QFileInfo::exists(e.upper()));
    }
    void importRefusesFifosAndOversizedFilesWithoutReadingThem() {
        Env e; const QString fifo = e.files.path() + "/fifo"; QVERIFY(::mkfifo(fifo.toUtf8().constData(), 0600) == 0);
        const auto args = [&](const QString &f) { return QStringList{"import", "--prefix", e.prefix.path(), "--runtime-root", e.runtime.path(), "--name", kLib, "--file", f}; };
        QElapsedTimer t; t.start();
        const Result a = run(args(fifo)); QCOMPARE(a.code, 1); QVERIFY(a.err.contains("not a regular file")); QVERIFY(t.elapsed() < 2000);
        const QString link = e.files.path() + "/link"; QVERIFY(QFile::link(e.image(), link));
        const Result c = run(args(link)); QCOMPARE(c.code, 1); QVERIFY2(c.err.contains("cannot open"), qPrintable(c.err));
        QVERIFY(!QFileInfo::exists(e.upper()));
        const QString big = e.files.path() + "/big"; QFile f(big); QVERIFY(f.open(QIODevice::WriteOnly)); QVERIFY(f.resize((qint64(512) << 20) + 1)); f.close();
        const Result b = run(args(big)); QCOMPARE(b.code, 1); QVERIFY(b.err.contains("larger than 512 MiB"));
    }
    void masterOnRefusedByTheRuntimeExitsOne() {
        Env e; QCOMPARE(e.import().code, 0); QCOMPARE(run(e.base("master") + QStringList{"off"}).code, 0);
        put(e.runtime.path() + kLib, "runtime");
        const Result r = run(e.base("master") + QStringList{"on", "--runtime-root", e.runtime.path()});
        QCOMPARE(r.code, 1); QVERIFY(r.err.contains("runtime")); QVERIFY(!QFileInfo::exists(e.upper()));
    }
    void disableEnableAbandonAndMasterVerbs() {
        Env e; QCOMPARE(e.import().code, 0);
        QCOMPARE(run(e.base("disable") + QStringList{"--name", kLib}).code, 0);
        QVERIFY(!QFileInfo::exists(e.upper())); QCOMPARE(e.list().out, kLib + "\tdisabled\tvolume\n");
        QCOMPARE(run(e.base("enable") + QStringList{"--runtime-root", e.runtime.path(), "--name", kLib}).code, 0);
        QCOMPARE(slurp(e.upper()), QByteArray("image-bytes"));
        QCOMPARE(run(e.base("master") + QStringList{"off"}).code, 0);
        QVERIFY(!QFileInfo::exists(e.upper()));
        QCOMPARE(run(e.base("enable") + QStringList{"--runtime-root", e.runtime.path(), "--name", kLib}).code, 1); // switched off
        QCOMPARE(run(e.base("master") + QStringList{"on", "--runtime-root", e.runtime.path()}).code, 0);
        QCOMPARE(slurp(e.upper()), QByteArray("image-bytes"));
        put(e.upper(), "tampered");
        QCOMPARE(run(e.base("disable") + QStringList{"--name", kLib}).code, 1);
        const Result ab = run(e.base("abandon") + QStringList{"--name", kLib});
        QCOMPARE(ab.code, 0); QCOMPARE(slurp(e.upper()), QByteArray("tampered")); QVERIFY(e.list().out.isEmpty());
    }
    void masterOffWarnsAboutFilesItLeftAlone() {
        Env e; QCOMPARE(e.import().code, 0); put(e.upper(), "tampered");
        const Result r = run(e.base("master") + QStringList{"off"});
        QCOMPARE(r.code, 3); QVERIFY(r.err.contains("warning") && r.err.contains(kLib));
    }
    void refusedEnableAndDisableExitOne() {
        Env e; QCOMPARE(e.import().code, 0); put(e.upper(), "tampered");
        const Result r = run(e.base("disable") + QStringList{"--name", kLib});
        QCOMPARE(r.code, 1); QVERIFY(r.err.startsWith("error: ")); QCOMPARE(slurp(e.upper()), QByteArray("tampered"));
    }
    void usageErrorsExitTwo_data() {
        QTest::addColumn<QStringList>("args");
        QTest::newRow("no verb") << QStringList{};
        QTest::newRow("unknown verb") << QStringList{"frobnicate", "--prefix", "/x"};
        QTest::newRow("no prefix") << QStringList{"list"};
        QTest::newRow("unknown option") << QStringList{"list", "--prefix", "/x", "--bogus"};
        QTest::newRow("option without value") << QStringList{"list", "--prefix"};
        QTest::newRow("unknown option on a verb with a name") << QStringList{"disable", "--prefix", "/x", "--name", "/usr/lib/a.dylib", "--bogus"};
        QTest::newRow("stray argument") << QStringList{"list", "--prefix", "/x", "extra"};
        QTest::newRow("import without file") << QStringList{"import", "--prefix", "/x", "--runtime-root", "/r", "--name", "/usr/lib/a.dylib"};
        QTest::newRow("bad source kind") << QStringList{"import", "--prefix", "/x", "--runtime-root", "/r", "--name", "/usr/lib/a.dylib", "--file", "/f", "--source-kind", "cache"};
        QTest::newRow("enable without runtime root") << QStringList{"enable", "--prefix", "/x", "--name", "/usr/lib/a.dylib"};
        QTest::newRow("disable without name") << QStringList{"disable", "--prefix", "/x"};
        QTest::newRow("abandon without name") << QStringList{"abandon", "--prefix", "/x"};
        QTest::newRow("master without state") << QStringList{"master", "--prefix", "/x"};
        QTest::newRow("master bad state") << QStringList{"master", "maybe", "--prefix", "/x"};
        QTest::newRow("master on without runtime root") << QStringList{"master", "on", "--prefix", "/x"};
    }
    void usageErrorsExitTwo() { QFETCH(QStringList, args); const Result r = run(args); QCOMPARE(r.code, 2); QVERIFY(r.err.contains("usage error")); QVERIFY(r.out.isEmpty()); }
    void helpPrintsUsageAndExitsZero() {
        for (const char *flag : {"--help", "-h", "help"}) {
            const Result r = run({flag}); QCOMPARE(r.code, 0); QVERIFY(r.out.contains("usage:") && r.out.contains("exit codes")); QVERIFY(r.err.isEmpty());
        }
        QVERIFY(!run({"list"}).err.contains("libscli.h"));
    }
    void listFailsClosedOnACorruptRecordFile() {
        Env e; put(e.prefix.path() + "/.darling-launcher/libraries.json", "garbage");
        const Result r = e.list(); QVERIFY2(r.code == 1, qPrintable(r.err));
    }
};
QTEST_APPLESS_MAIN(LibsCliTest)
#include "libs_cli_test.moc"
