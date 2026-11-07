// SPDX-License-Identifier: GPL-3.0-or-later
#include "import_env.h"
#include "libraryops.h"
#include "librarystatus.h"

struct StatusEnv : Env {
    QString status(const QString &name = kLib, QString *errOut = nullptr) {
        LibraryStatus s; QString err;
        const bool ok = libraryStatus(prefix.path(), runtime.path(), name, &s, &err);
        if (errOut) *errOut = err;
        return ok ? libraryStatusName(s) : "error";
    }
    bool disable() { QString e; return disableLibrary(prefix.path(), kLib, &e, opts); }
    bool master(bool on) { QString e; return setMasterSwitch(prefix.path(), runtime.path(), on, false, nullptr, &e, opts); }
};

class StatusTest : public QObject {
    Q_OBJECT
private slots:
    void missingThenImported() {
        StatusEnv e; QCOMPARE(e.status(), QString("missing"));
        QVERIFY(e.import(kLib, "img")); QCOMPARE(e.status(), QString("imported"));
    }
    void builtinFollowsTheRuntimeFile() {
        StatusEnv e; put(e.runtime.path() + kLib, "runtime");
        QCOMPARE(e.status(), QString("builtin"));
        QVERIFY(QFile::remove(e.runtime.path() + kLib));
        QCOMPARE(e.status(), QString("missing"));
    }
    void importedWinsOverTheRuntimeCopyItShadows() {
        StatusEnv e; put(e.runtime.path() + kLib, "runtime");
        QVERIFY(e.import(kLib, "img", true));
        QCOMPARE(e.status(), QString("imported"));
    }
    void disabledAfterDisableAndAfterTheMasterSwitch() {
        StatusEnv e; QVERIFY(e.import(kLib, "img")); QVERIFY(e.disable());
        QCOMPARE(e.status(), QString("disabled"));
        StatusEnv f; QVERIFY(f.import(kLib, "img")); QVERIFY(f.master(false));
        QCOMPARE(f.status(), QString("disabled"));
        QVERIFY(f.master(true)); QCOMPARE(f.status(), QString("imported"));
    }
    void disabledWithTheRuntimeCopyIsNotBuiltin() {
        StatusEnv e; put(e.runtime.path() + kLib, "runtime"); QVERIFY(e.import(kLib, "img", true)); QVERIFY(e.disable());
        QCOMPARE(e.status(), QString("disabled"));
    }
    void interruptedImportAndInterruptedDisable() {
        StatusEnv e; e.opts.abortAfter = [](const QString &s) { return s == "upper"; };
        QVERIFY(!e.import(kLib, "img")); QCOMPARE(e.status(), QString("interrupted"));
        e.opts.abortAfter = nullptr; QVERIFY(e.import(kLib, "img")); QCOMPARE(e.status(), QString("imported"));
        e.opts.abortAfter = [](const QString &s) { return s == "removed"; };
        QVERIFY(!e.disable()); QCOMPARE(e.status(), QString("interrupted")); // record enabled, file gone
    }
    void interruptedReplaceOfAnEnabledLibraryIsInterrupted() {
        StatusEnv e; QVERIFY(e.import(kLib, "old"));
        e.opts.abortAfter = [](const QString &s) { return s == "upper"; };
        QVERIFY(!e.import(kLib, "new")); QCOMPARE(e.status(), QString("interrupted"));
    }
    void abandoningATamperedLibraryLeavesAForeignFile() {
        StatusEnv e; QVERIFY(e.import(kLib, "img")); put(e.upper(), "tampered");
        QString err; QVERIFY(abandonLibrary(e.prefix.path(), kLib, &err, e.opts));
        QCOMPARE(e.status(), QString("foreign"));
        QVERIFY(QFile::remove(e.upper())); QCOMPARE(e.status(), QString("missing"));
    }
    void aFileWeDidNotWriteIsForeign() {
        StatusEnv e; put(e.upper(), "guest"); QCOMPARE(e.status(), QString("foreign"));
        StatusEnv f; QVERIFY(f.import(kLib, "img")); put(f.upper(), "tampered"); QCOMPARE(f.status(), QString("foreign"));
    }
    void failsClosedOnBadInput() {
        StatusEnv e; QString err;
        QCOMPARE(e.status("@rpath/X", &err), QString("error"));
        LibraryStatus s;
        QVERIFY(!libraryStatus(e.prefix.path(), "relative", kLib, &s, &err));
        put(e.prefix.path() + "/.darling-launcher/libraries.json", "garbage");
        QCOMPARE(e.status(kLib, &err), QString("error"));
    }
};
QTEST_APPLESS_MAIN(StatusTest)
#include "librarystatus_test.moc"
