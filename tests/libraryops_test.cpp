// SPDX-License-Identifier: GPL-3.0-or-later
#include "import_env.h"
#include "libraryops.h"

static const QString kBar = "/usr/lib/libbar.dylib";

struct OpsEnv : Env {
    bool disable(const QString &n = kLib, QString *err = nullptr) { QString l; return disableLibrary(prefix.path(), n, err ? err : &l, opts); }
    bool enable(const QString &n = kLib, bool force = false, QString *err = nullptr) {
        QString l; return enableLibrary(prefix.path(), runtime.path(), n, force, err ? err : &l, opts);
    }
    QStringList skipped;
    bool master(bool on, bool force = false, QString *err = nullptr) {
        QString l; return setMasterSwitch(prefix.path(), runtime.path(), on, force, &skipped, err ? err : &l, opts);
    }
    bool abandon(const QString &n = kLib, QString *err = nullptr) { QString l; return abandonLibrary(prefix.path(), n, err ? err : &l, opts); }
    bool has(const QString &n = kLib) const { return QFileInfo::exists(upper(n)); }
    QString store(const QString &n = kLib) const { return Libraries::storePath(prefix.path(), Libraries::idFor(n)); }
    bool recordEnabled(const QString &n = kLib) { for (const auto &r : records().libraries) if (r.installName == n) return r.enabled; return false; }
    void markRunning() {
        put(prefix.path() + "/.init.pid", "4242"); QDir().mkpath(proc.path() + "/4242");
        QVERIFY(QFile::link("/x/darlingserver", proc.path() + "/4242/exe"));
    }
};

class OpsTest : public QObject {
    Q_OBJECT
private slots:
    void disableRemovesOurFileAndIsIdempotent() {
        OpsEnv e; QVERIFY(e.import(kLib, "img")); QString err;
        QVERIFY2(e.disable(kLib, &err), qPrintable(err));
        QVERIFY(!e.has()); QVERIFY(!e.recordEnabled());
        QCOMPARE(slurp(e.store() + "/image"), QByteArray("img"));
        QVERIFY(e.disable());
    }
    void enableRestoresFromTheStoreAndIsIdempotent() {
        OpsEnv e; QVERIFY(e.import(kLib, "img")); QVERIFY(e.disable());
        QString err; QVERIFY2(e.enable(kLib, false, &err), qPrintable(err));
        QCOMPARE(slurp(e.upper()), QByteArray("img")); QCOMPARE(modeOf(e.upper()), 0644); QVERIFY(e.recordEnabled());
        QVERIFY(e.enable());
    }
    void enableOfAnEnabledLibraryDoesNotRewriteItAndUnknownLibrariesAreRefused() {
        OpsEnv e; QVERIFY(e.import(kLib, "img")); struct stat a, b;
        QVERIFY(::stat(e.upper().toUtf8().constData(), &a) == 0);
        QVERIFY(e.enable());
        QVERIFY(::stat(e.upper().toUtf8().constData(), &b) == 0); QCOMPARE(quint64(b.st_ino), quint64(a.st_ino));
        QString err; QVERIFY(!e.enable(kBar, false, &err)); QVERIFY(err.contains("not imported"));
    }
    void disableRefusesATamperedUpperFile() {
        OpsEnv e; QVERIFY(e.import(kLib, "img")); put(e.upper(), "tampered"); QString err;
        QVERIFY(!e.disable(kLib, &err)); QVERIFY(err.contains("did not write"));
        QCOMPARE(slurp(e.upper()), QByteArray("tampered")); QVERIFY(e.recordEnabled());
    }
    void disableIsIdempotentWhenThereIsNothingOfOursToRemove() {
        OpsEnv e; QVERIFY(e.disable()); // no record, no file
        put(e.upper(), "guest"); QVERIFY(e.disable()); QCOMPARE(slurp(e.upper()), QByteArray("guest")); // foreign, no record
        OpsEnv f; QVERIFY(f.import(kLib, "img")); QVERIFY(f.disable()); put(f.upper(), "guest");
        QVERIFY(f.disable()); QCOMPARE(slurp(f.upper()), QByteArray("guest")); // disabled record, foreign file
    }
    void abandonDropsTheRecordAndOurStoreButNeverTheUpperFile() {
        OpsEnv e; QVERIFY(e.import(kLib, "img")); put(e.upper(), "tampered");
        QVERIFY(!e.disable());
        QString err; QVERIFY2(e.abandon(kLib, &err), qPrintable(err));
        QCOMPARE(slurp(e.upper()), QByteArray("tampered"));
        QVERIFY(e.records().libraries.isEmpty()); QVERIFY(!QFileInfo::exists(e.store()));
        QVERIFY(e.abandon()); // idempotent
    }
    void abandonRefusesWhileTheUpperFileIsStillOursAndWhileRunning() {
        OpsEnv e; QVERIFY(e.import(kLib, "img")); QString err;
        QVERIFY(!e.abandon(kLib, &err)); QVERIFY(err.contains("disable")); QCOMPARE(e.records().libraries.size(), 1);
        e.markRunning(); put(e.upper(), "tampered");
        QVERIFY(!e.abandon(kLib, &err)); QVERIFY(err.contains("running")); QCOMPARE(e.records().libraries.size(), 1);
    }
    void disableWorksAtEveryCrashState_data() {
        QTest::addColumn<QString>("stage"); QTest::addColumn<bool>("replace");
        for (const char *st : {"pending", "upper", "record"}) for (bool r : {false, true})
            if (r || QString(st) != "pending") QTest::newRow(qPrintable(QString("%1 after %2").arg(r ? "replace" : "fresh", st))) << QString(st) << r;
    }
    void disableWorksAtEveryCrashState() {
        QFETCH(QString, stage); QFETCH(bool, replace);
        OpsEnv e; if (replace) QVERIFY(e.import(kLib, "old"));
        e.opts.abortAfter = [&](const QString &s) { return s == stage; };
        QVERIFY(!e.import(kLib, "new")); e.opts.abortAfter = nullptr;
        QString err; QVERIFY2(e.disable(kLib, &err), qPrintable(err));
        QVERIFY(!e.has());
        for (const auto &r : e.records().libraries) QVERIFY(!r.enabled);
    }
    void enableRestoresFromPendingWhenTheImageIsStaleOrMissing() {
        OpsEnv e; QVERIFY(e.import(kLib, "old"));
        e.opts.abortAfter = [](const QString &s) { return s == "record"; };
        QVERIFY(!e.import(kLib, "new")); e.opts.abortAfter = nullptr; // record=new, image=old, pending=new
        QVERIFY(e.disable()); QString err; QVERIFY2(e.enable(kLib, false, &err), qPrintable(err));
        QCOMPARE(slurp(e.upper()), QByteArray("new"));
    }
    void enableRepairsAnInterruptedReplaceToTheRecordedContent() {
        OpsEnv e; QVERIFY(e.import(kLib, "old"));
        e.opts.abortAfter = [](const QString &s) { return s == "upper"; };
        QVERIFY(!e.import(kLib, "new")); e.opts.abortAfter = nullptr; // upper=new, record=old
        QString err; QVERIFY2(e.enable(kLib, false, &err), qPrintable(err));
        QCOMPARE(slurp(e.upper()), QByteArray("old"));
    }
    void enableRefusesATamperedStoreCopy() {
        OpsEnv e; QVERIFY(e.import(kLib, "img")); QVERIFY(e.disable());
        put(e.store() + "/image", "tampered"); QString err;
        QVERIFY(!e.enable(kLib, false, &err)); QVERIFY(err.contains("recorded sha256")); QVERIFY(!e.has()); QVERIFY(!e.recordEnabled());
    }
    void enableRefusesAForeignFileAtTheDestination() {
        OpsEnv e; QVERIFY(e.import(kLib, "img")); QVERIFY(e.disable()); put(e.upper(), "guest");
        QVERIFY(!e.enable()); QCOMPARE(slurp(e.upper()), QByteArray("guest"));
    }
    void enableRefusesRuntimeProvidedAndPinnedPathsUnlessForced() {
        OpsEnv e; QVERIFY(e.import(kLib, "img")); QVERIFY(e.disable());
        put(e.runtime.path() + kLib, "runtime"); QString err;
        QVERIFY(!e.enable(kLib, false, &err)); QVERIFY(err.contains("runtime")); QVERIFY(!e.has());
        QVERIFY(e.enable(kLib, true)); QVERIFY(e.has());
        QVERIFY(e.import(kAppKit, "pinned", true)); QVERIFY(e.disable(kAppKit));
        QVERIFY(!e.enable(kAppKit)); QVERIFY(e.enable(kAppKit, true));
    }
    void runningPrefixAndHeldLockAreRefused() {
        OpsEnv e; QVERIFY(e.import(kLib, "img")); e.markRunning(); QString err;
        QVERIFY(!e.disable(kLib, &err)); QVERIFY(err.contains("running")); QVERIFY(e.has());
        QVERIFY(!e.enable()); QVERIFY(!e.master(false)); QVERIFY(e.has());
        OpsEnv f; QVERIFY(f.import(kLib, "img"));
        const int fd = SafeFs::lockFile(f.prefix.path(), ".darling-launcher/lock", 0755, &err); QVERIFY(fd >= 0);
        QVERIFY(!f.disable(kLib, &err)); QVERIFY(err.contains("Another launcher")); ::close(fd);
    }
    void interruptedDisableAndEnableAreCompletedByRerunning() {
        OpsEnv e; QVERIFY(e.import(kLib, "img"));
        e.opts.abortAfter = [](const QString &s) { return s == "removed"; };
        QVERIFY(!e.disable()); QVERIFY(!e.has()); QVERIFY(e.recordEnabled());
        e.opts.abortAfter = nullptr; QVERIFY(e.disable()); QVERIFY(!e.recordEnabled());
        e.opts.abortAfter = [](const QString &s) { return s == "upper"; };
        QVERIFY(!e.enable()); QVERIFY(e.has()); QVERIFY(!e.recordEnabled());
        e.opts.abortAfter = nullptr; QVERIFY(e.enable()); QVERIFY(e.recordEnabled());
    }
    void masterSwitchRoundTripKeepsPerLibraryFlags() {
        OpsEnv e; QVERIFY(e.import(kLib, "a")); QVERIFY(e.import(kBar, "b")); QVERIFY(e.disable(kBar));
        QString err; QVERIFY2(e.master(false, false, &err), qPrintable(err));
        QVERIFY(!e.has() && !e.has(kBar)); QVERIFY(!e.records().enabled);
        QVERIFY(e.recordEnabled(kLib)); QVERIFY(!e.recordEnabled(kBar));
        QVERIFY(!e.enable(kLib, false, &err)); QVERIFY(err.contains("switched off"));
        put(e.upper(), "guest"); QVERIFY(e.master(false)); // idempotent: an already-off switch touches nothing
        QCOMPARE(slurp(e.upper()), QByteArray("guest")); QVERIFY(QFile::remove(e.upper()));
        QVERIFY2(e.master(true, false, &err), qPrintable(err));
        QCOMPARE(slurp(e.upper()), QByteArray("a")); QVERIFY(!e.has(kBar)); QVERIFY(!e.recordEnabled(kBar));
        QVERIFY(e.records().enabled);
    }
    void masterOffSkipsAndReportsFilesThatAreNotOurs() {
        OpsEnv e; QVERIFY(e.import(kLib, "a")); QVERIFY(e.import(kBar, "b")); put(e.upper(kBar), "tampered");
        QString err; QVERIFY2(e.master(false, false, &err), qPrintable(err));
        QVERIFY(!e.has(kLib)); QCOMPARE(slurp(e.upper(kBar)), QByteArray("tampered"));
        QCOMPARE(e.skipped, QStringList({kBar})); QVERIFY(!e.records().enabled);
        QVERIFY(e.recordEnabled(kLib) && e.recordEnabled(kBar)); // per-library flags intact
    }
    void masterOffStillRefusesOnIoErrors() {
        OpsEnv e; QVERIFY(e.import(kLib, "a")); QVERIFY(QFile::remove(e.upper()));
        QVERIFY(QFile::link(e.evil.path(), e.upper())); // a symlink at the path: unreadable as a regular file
        QVERIFY(!e.master(false)); QVERIFY(e.records().enabled);
    }
    void libraryFailingMidLoopOfTheMasterSwitchIsCompletedByRerunning() {
        OpsEnv e; QVERIFY(e.import(kLib, "a")); QVERIFY(e.import(kBar, "b"));
        e.opts.abortAfter = [](const QString &s) { return s == "item:" + kLib; };
        QVERIFY(!e.master(false));
        QVERIFY(!e.has(kLib)); QVERIFY(e.has(kBar)); QVERIFY(e.records().enabled); // half done, record consistent
        e.opts.abortAfter = nullptr; QVERIFY(e.master(false));
        QVERIFY(!e.has(kBar)); QVERIFY(!e.records().enabled); QVERIFY(e.recordEnabled(kLib) && e.recordEnabled(kBar));
        e.opts.abortAfter = [](const QString &s) { return s == "item:" + kLib; };
        QVERIFY(!e.master(true)); QVERIFY(e.has(kLib)); QVERIFY(!e.has(kBar)); QVERIFY(!e.records().enabled);
        e.opts.abortAfter = nullptr; QVERIFY(e.master(true));
        QCOMPARE(slurp(e.upper(kBar)), QByteArray("b")); QVERIFY(e.records().enabled);
    }
    void masterOnChangesNothingWhenTheRuntimeNowProvidesAPath() {
        OpsEnv e; QVERIFY(e.import(kLib, "a")); QVERIFY(e.import(kBar, "b")); QVERIFY(e.master(false));
        put(e.runtime.path() + kBar, "runtime"); QString err;
        QVERIFY(!e.master(true, false, &err)); QVERIFY(!e.has(kLib) && !e.has(kBar)); QVERIFY(!e.records().enabled);
        QVERIFY(e.master(true, true));
    }
    void interruptedMasterSwitchIsCompletedByRerunning() {
        OpsEnv e; QVERIFY(e.import(kLib, "a"));
        e.opts.abortAfter = [](const QString &s) { return s == "removed"; };
        QVERIFY(!e.master(false)); QVERIFY(!e.has()); QVERIFY(e.records().enabled);
        e.opts.abortAfter = nullptr; QVERIFY(e.master(false)); QVERIFY(!e.records().enabled);
        e.opts.abortAfter = [](const QString &s) { return s == "upper"; };
        QVERIFY(!e.master(true)); QVERIFY(e.has()); QVERIFY(!e.records().enabled);
        e.opts.abortAfter = nullptr; QVERIFY(e.master(true)); QVERIFY(e.records().enabled);
    }
};
QTEST_APPLESS_MAIN(OpsTest)
#include "libraryops_test.moc"
