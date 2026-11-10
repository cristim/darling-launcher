// SPDX-License-Identifier: GPL-3.0-or-later
#include "dyldcache_fixture.h"
#include "dyldcachecli.h"
#include <QProcess>
#include <QTemporaryDir>
#include <csignal>
#include <sys/stat.h>
#include <unistd.h>

static const QString kLibz = "/usr/lib/libz.1.dylib";
static const QString kMain = "dyld_shared_cache_arm64e";
static QByteArray slurp(const QString &p) { QFile f(p); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }

struct Result { int code; QString out, err; };
static Result run(const QStringList &args) {
    QString o, e; QTextStream out(&o), err(&e);
    const int code = dyldCacheMain(args, out, err);
    return {code, o, e};
}

struct Fx {
    QTemporaryDir src, work, prefix;
    QStringList subs;
    QByteArray uuid;
    explicit Fx(int seed = 1, int subCount = 12, qint64 fileSize = 4096) {
        for (int i = 1; i <= subCount; ++i) subs << QString(".%1").arg(i, 2, 10, QChar('0'));
        uuid = makeFakeCache(src.path(), "arm64e", seed, subs, fileSize);
    }
    QString root() const { return work.path() + "/store"; }
    QString id() const { return DyldCacheStore::storeId("arm64e", DyldCache::uuidHex(uuid)); }
    QStringList names() const { QStringList n{kMain}; for (const QString &s : subs) n << kMain + s; return n; }
    QStringList copyArgs() const { return {"copy", "--source", src.path(), "--store-root", root(), "--margin-mib", "0"}; }
    QStringList rootArg(const QString &verb) const { return {verb, "--store-root", root()}; }
    QString file(const QString &name) const { return root() + "/" + id() + "/" + name; }
};

class DyldCacheCliTest : public QObject {
    Q_OBJECT
private slots:
    void copiesTheThirteenFilesAndReportsProgressWithoutPaths() {
        Fx fx;
        const Result r = run(fx.copyArgs());
        QCOMPARE(r.code, 0); QVERIFY(r.out.isEmpty());
        QCOMPARE(fx.names().size(), 13);
        for (const QString &n : fx.names()) QCOMPARE(slurp(fx.file(n)), slurp(fx.src.path() + "/" + n));
        QVERIFY2(r.err.contains("[1/13] " + kMain) && r.err.contains("[13/13] "), qPrintable(r.err));
        QVERIFY2(r.err.contains("done: store " + fx.id()), qPrintable(r.err));
        QVERIFY2(!r.err.contains(fx.root()) && !r.err.contains(fx.src.path()), qPrintable(r.err));
        QCOMPARE(run(fx.copyArgs()).code, 0); // a ready store is reported done again
        QCOMPARE(run(fx.rootArg("verify")).out, "ok\t" + fx.id() + "\n");
    }
    void failuresExitOneWithOnePathFreeLine() {
        Fx fx;
        QFile::remove(fx.src.path() + "/" + kMain + ".05");
        const Result r = run(fx.copyArgs());
        QCOMPARE(r.code, 1); QVERIFY(r.out.isEmpty());
        QVERIFY2(r.err.startsWith("error: ") && r.err.count('\n') == 1, qPrintable(r.err));
        QVERIFY2(!r.err.contains(fx.root()) && !r.err.contains(fx.src.path()), qPrintable(r.err));
        QVERIFY(!QFileInfo::exists(fx.file("store.json")));
        QTemporaryDir empty;
        const Result none = run({"copy", "--source", empty.path(), "--store-root", fx.root(), "--margin-mib", "0"});
        QCOMPARE(none.code, 1); QVERIFY2(!none.err.contains(empty.path()) && none.err.count('\n') == 1, qPrintable(none.err));
        QCOMPARE(run({"copy", "--source", fx.src.path(), "--arch", "arm64", "--store-root", fx.root()}).code, 1); // no arm64 cache there
        Fx whole; // the first fixture lost a file
        const Result space = run({"copy", "--source", whole.src.path(), "--store-root", whole.root(), "--margin-mib", "1048576"}); // a 1 TiB margin cannot be free
        QCOMPARE(space.code, 1); QVERIFY2(space.err.contains("Not enough free space"), qPrintable(space.err));
    }
    void sigintStopsCleanlyKeepsThePartialAndTheNextRunResumes() {
        Fx fx(2, 12, qint64(3) << 20);
        QProcess p;
        p.setReadChannel(QProcess::StandardError);
        p.start(LAUNCHER_BIN, QStringList{"dyld-cache"} + fx.copyArgs() + QStringList{"--window-mib", "1", "--throttle-ms", "100"});
        QVERIFY(p.waitForStarted());
        QByteArray errText;
        QElapsedTimer waited; waited.start();
        while (!errText.contains("[3/13]") && waited.elapsed() < 20000 && p.state() != QProcess::NotRunning) {
            p.waitForReadyRead(50);
            errText += p.readAllStandardError();
        }
        QVERIFY2(errText.contains("[3/13]"), errText.constData());
        ::kill(pid_t(p.processId()), SIGINT);
        QVERIFY(p.waitForFinished(20000));
        errText += p.readAllStandardError();
        QCOMPARE(p.exitStatus(), QProcess::NormalExit); QCOMPARE(p.exitCode(), 130);
        QVERIFY2(errText.contains("cancelled") && errText.contains("resume"), errText.constData());
        QVERIFY(!QFileInfo::exists(fx.file("store.json")));
        QVERIFY(QFileInfo::exists(fx.file(kMain))); QVERIFY(QFileInfo::exists(fx.file(kMain + ".01")));
        QStringList partials;
        for (const QString &n : QDir(fx.root() + "/" + fx.id()).entryList({"*.partial"})) partials << n;
        QCOMPARE(partials.size(), 1);
        QVERIFY(!QFileInfo::exists(fx.file(kMain + ".12")));
        const Result r = run(fx.copyArgs());
        QCOMPARE(r.code, 0); QVERIFY2(r.err.contains("done: store"), qPrintable(r.err));
        for (const QString &n : fx.names()) QCOMPARE(slurp(fx.file(n)), slurp(fx.src.path() + "/" + n));
        QVERIFY(QDir(fx.root() + "/" + fx.id()).entryList({"*.partial"}).isEmpty());
        QCOMPARE(run(fx.rootArg("verify")).code, 0);
    }
    void throttleAndWindowReachTheEngineAndProgressIsRateLimited() {
        Fx fx(3, 12, qint64(3) << 20);
        QElapsedTimer t; t.start();
        const Result r = run(fx.copyArgs() + QStringList{"--window-mib", "1", "--throttle-ms", "50"});
        const qint64 ms = t.elapsed();
        QCOMPARE(r.code, 0);
        QVERIFY2(ms >= 1200, qPrintable(QString::number(ms))); // 13 files, two pauses each (three 1 MiB windows), 50 ms per pause
        const int lines = r.err.count("\n[");
        QVERIFY2(lines >= 12 && lines <= 13 + ms / 1000 + 1, qPrintable(QString::number(lines) + " progress lines in " + QString::number(ms) + " ms")); // not one per MiB block
    }
    void statusIsFreshForTheSameSourceAndStaleForAnotherBuild() {
        Fx fx; QCOMPARE(run(fx.copyArgs()).code, 0);
        const Result plain = run(fx.rootArg("status"));
        QCOMPARE(plain.code, 0);
        QVERIFY2(plain.out.startsWith(fx.id() + "\tready\t13\t" + QString::number(13 * 4096) + "\t"), qPrintable(plain.out));
        QVERIFY(!plain.out.contains("source"));
        const Result fresh = run(fx.rootArg("status") + QStringList{"--source", fx.src.path()});
        QCOMPARE(fresh.code, 0);
        QVERIFY2(fresh.out.contains("\tcurrent\n") && !fresh.out.contains("stale"), qPrintable(fresh.out));
        QVERIFY2(fresh.out.contains("source\t" + fx.id() + "\tstored\n"), qPrintable(fresh.out));
        Fx newer(7); // another macOS build: another UUID
        const Result stale = run(fx.rootArg("status") + QStringList{"--source", newer.src.path()});
        QCOMPARE(stale.code, 0);
        QVERIFY2(stale.out.contains(fx.id() + "\tready\t13") && stale.out.contains("\tstale\n"), qPrintable(stale.out));
        QVERIFY2(stale.out.contains("source\t" + newer.id() + "\tnot stored (stale)\n"), qPrintable(stale.out));
        QVERIFY2(!stale.out.contains(fx.root()) && !stale.out.contains(newer.src.path()) && stale.err.isEmpty(), qPrintable(stale.out));
        QTemporaryDir empty;
        QCOMPARE(run(fx.rootArg("status") + QStringList{"--source", empty.path()}).code, 1); // unreadable source is not "stale"
    }
    void statusWithoutAStoreSaysSo() {
        Fx fx;
        const Result r = run(fx.rootArg("status") + QStringList{"--source", fx.src.path()});
        QCOMPARE(r.code, 0); QCOMPARE(r.out, QString("no store\nsource\t" + fx.id() + "\tnot stored (stale)\n"));
    }
    void verifyDetectsAFlippedByteAndNamesTheFile() {
        Fx fx; QCOMPARE(run(fx.copyArgs()).code, 0);
        const QString victim = fx.file(kMain + ".07");
        QVERIFY(QFile::setPermissions(victim, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        QFile f(victim); QVERIFY(f.open(QIODevice::ReadWrite));
        QVERIFY(f.seek(2000)); char c; QVERIFY(f.getChar(&c)); QVERIFY(f.seek(2000)); QVERIFY(f.putChar(char(c ^ 1))); f.close();
        const Result r = run(fx.rootArg("verify"));
        QCOMPARE(r.code, 1); QVERIFY(r.out.isEmpty());
        QVERIFY2(r.err.contains(kMain + ".07 differs from the recorded checksum") && r.err.contains(fx.id()), qPrintable(r.err));
        QVERIFY2(!r.err.contains(fx.root()), qPrintable(r.err));
        QCOMPARE(run(fx.rootArg("verify") + QStringList{"--store", fx.id()}).code, 1);
        QTemporaryDir nothing;
        QCOMPARE(run({"verify", "--store-root", nothing.path() + "/none"}).code, 1);
        QCOMPARE(run(fx.rootArg("verify") + QStringList{"--store", "../x"}).code, 1);
    }
    void prefixEnableWritesConfigAndPolicyAndDisableKeepsTheSelection() {
        Fx fx; QCOMPARE(run(fx.copyArgs()).code, 0);
        const QString cfg = fx.prefix.path() + "/.darling-launcher/dyld-cache.json", pol = fx.prefix.path() + "/.darling-launcher/dyld-cache.policy";
        const QStringList base = {"prefix", "--prefix", fx.prefix.path(), "--store-root", fx.root()};
        const Result on = run(base + QStringList{"--enable"});
        QCOMPARE(on.code, 0); QCOMPARE(on.out, QString("ok enabled\n"));
        QCOMPARE(slurp(pol), QByteArray("darling-dyld-cache-policy 1\n/usr/lib/libz.1.dylib\n"));
        const QByteArray json = slurp(cfg);
        QVERIFY2(json.contains("\"mode\": \"allowlist\"") && json.contains(fx.id().toUtf8()) && json.contains("libz.1.dylib"), json.constData());
        QCOMPARE(run(base + QStringList{"--enable", "--lib", kLibz, "--store", fx.id()}).code, 0); // explicit form, same result
        const Result off = run(base + QStringList{"--disable"});
        QCOMPARE(off.code, 0); QCOMPARE(off.out, QString("ok disabled\n"));
        QVERIFY(!QFileInfo::exists(pol));
        const QByteArray offJson = slurp(cfg);
        QVERIFY2(offJson.contains("\"mode\": \"off\"") && offJson.contains("libz.1.dylib") && offJson.contains(fx.id().toUtf8()), offJson.constData());
        QCOMPARE(run(base + QStringList{"--enable"}).code, 0); // the kept selection comes back
        QVERIFY(QFileInfo::exists(pol));
    }
    void prefixRefusesALibraryThatIsNotValidated() {
        Fx fx; QCOMPARE(run(fx.copyArgs()).code, 0);
        const QString cfg = fx.prefix.path() + "/.darling-launcher/dyld-cache.json", pol = fx.prefix.path() + "/.darling-launcher/dyld-cache.policy";
        const QStringList base = {"prefix", "--prefix", fx.prefix.path(), "--store-root", fx.root(), "--enable"};
        for (const QString &lib : {QString("/usr/lib/libSystem.B.dylib"), QString("@rpath/libz.1.dylib"), QString("/usr/lib/../../x")}) {
            const Result r = run(base + QStringList{"--lib", lib});
            QCOMPARE(r.code, 1); QVERIFY2(r.err.startsWith("error: ") && r.err.count('\n') == 1 && r.out.isEmpty(), qPrintable(r.err));
            QVERIFY(!QFileInfo::exists(cfg)); QVERIFY(!QFileInfo::exists(pol));
        }
        QVERIFY2(run(base + QStringList{"--lib", "/usr/lib/libSystem.B.dylib"}).err.contains("not validated"), "message");
    }
    void controlBytesInArgumentsAreEscapedSoErrorsStayOneLine() {
        Fx fx; QCOMPARE(run(fx.copyArgs()).code, 0);
        const Result lib = run({"prefix", "--prefix", fx.prefix.path(), "--store-root", fx.root(), "--enable", "--lib", "/usr/lib/a\nerror: fake\tb"});
        QCOMPARE(lib.code, 1); QCOMPARE(lib.err.count('\n'), 1); QVERIFY2(lib.err.contains("\\x0a") && lib.err.contains("\\x09"), qPrintable(lib.err));
        const Result id = run(fx.rootArg("verify") + QStringList{"--store", "arm64e-x\ny"});
        QCOMPARE(id.code, 1); QCOMPARE(id.err.count('\n'), 1); QVERIFY2(id.err.contains("\\x0a"), qPrintable(id.err));
        const Result verb = run({"frob\nnicate"});
        QCOMPARE(verb.code, 2); QVERIFY2(verb.err.startsWith("usage error: unknown verb frob\\x0anicate\n"), qPrintable(verb.err));
    }
    void prefixRefusesAMissingOrAmbiguousStoreAndARunningPrefix() {
        Fx fx;
        const QString cfg = fx.prefix.path() + "/.darling-launcher/dyld-cache.json";
        const QStringList base = {"prefix", "--prefix", fx.prefix.path(), "--store-root", fx.root(), "--enable"};
        QCOMPARE(run(base).code, 1); QCOMPARE(run(base + QStringList{"--store", fx.id()}).code, 1); // nothing copied yet
        QCOMPARE(run(fx.copyArgs()).code, 0);
        Fx other(9); QCOMPARE(run({"copy", "--source", other.src.path(), "--store-root", fx.root(), "--margin-mib", "0"}).code, 0);
        const Result amb = run(base); QCOMPARE(amb.code, 1); QVERIFY2(amb.err.contains("--store"), qPrintable(amb.err));
        QVERIFY(!QFileInfo::exists(cfg));
        QFile pid(fx.prefix.path() + "/.init.pid"); QVERIFY(pid.open(QIODevice::WriteOnly)); pid.write(QByteArray::number(qint64(::getpid()))); pid.close();
        const Result busy = run(base + QStringList{"--store", fx.id()}); QCOMPARE(busy.code, 1); QVERIFY2(busy.err.contains("running"), qPrintable(busy.err));
        QVERIFY(!QFileInfo::exists(cfg));
    }
    void usageErrorsExitTwoAndHelpExitsZero_data() {
        QTest::addColumn<QStringList>("args");
        QTest::newRow("no verb") << QStringList{};
        QTest::newRow("unknown verb") << QStringList{"frobnicate"};
        QTest::newRow("copy without source") << QStringList{"copy"};
        QTest::newRow("unknown option") << QStringList{"verify", "--bogus"};
        QTest::newRow("option without value") << QStringList{"status", "--source"};
        QTest::newRow("bad window") << QStringList{"copy", "--source", "/x", "--window-mib", "0"};
        QTest::newRow("bad throttle") << QStringList{"copy", "--source", "/x", "--throttle-ms", "-1"};
        QTest::newRow("prefix without prefix") << QStringList{"prefix", "--enable"};
        QTest::newRow("prefix without mode") << QStringList{"prefix", "--prefix", "/x"};
        QTest::newRow("prefix both modes") << QStringList{"prefix", "--prefix", "/x", "--enable", "--disable"};
        QTest::newRow("disable with lib") << QStringList{"prefix", "--prefix", "/x", "--disable", "--lib", "/usr/lib/libz.1.dylib"};
        QTest::newRow("option of another verb") << QStringList{"verify", "--source", "/x"};
        QTest::newRow("enable on copy") << QStringList{"copy", "--source", "/x", "--enable"};
        QTest::newRow("empty prefix") << QStringList{"prefix", "--prefix", "", "--enable"};
        QTest::newRow("empty source") << QStringList{"copy", "--source", ""};
        QTest::newRow("empty store root") << QStringList{"status", "--store-root", ""};
        QTest::newRow("empty lib") << QStringList{"prefix", "--prefix", "/x", "--enable", "--lib", ""};
        QTest::newRow("repeated source") << QStringList{"status", "--source", "/a", "--source", "/b"};
        QTest::newRow("repeated store root") << QStringList{"verify", "--store-root", "/a", "--store-root", "/b"};
        QTest::newRow("bad arch on copy") << QStringList{"copy", "--source", "/x", "--arch", "x86_64"};
        QTest::newRow("bad arch on status") << QStringList{"status", "--arch", "x86_64"};
        QTest::newRow("throttle too long") << QStringList{"copy", "--source", "/x", "--throttle-ms", "60001"};
        QTest::newRow("window not a number") << QStringList{"copy", "--source", "/x", "--window-mib", "1x"};
    }
    void usageErrorsExitTwoAndHelpExitsZero() {
        QFETCH(QStringList, args);
        const Result r = run(args);
        QCOMPARE(r.code, 2); QVERIFY(r.err.startsWith("usage error: ")); QVERIFY(r.out.isEmpty());
        const Result h = run({"--help"}); QCOMPARE(h.code, 0); QVERIFY(h.out.startsWith("usage: darling-launcher dyld-cache"));
    }
};
QTEST_MAIN(DyldCacheCliTest)
#include "dyldcache_cli_test.moc"
