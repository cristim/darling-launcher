// SPDX-License-Identifier: GPL-3.0-or-later
#include "import_env.h"

class ImportTest : public QObject {
    Q_OBJECT
private slots:
    void writesStoreRecordAndUpperFile() {
        Env e; QString err;
        QVERIFY2(e.import(kLib, "image-bytes", false, &err), qPrintable(err));
        QCOMPARE(slurp(e.upper()), QByteArray("image-bytes"));
        const QString id = Libraries::idFor(kLib);
        QCOMPARE(slurp(Libraries::storePath(e.prefix.path(), id) + "/image"), QByteArray("image-bytes"));
        QCOMPARE(modeOf(Libraries::storePath(e.prefix.path(), id) + "/image"), 0600);
        QCOMPARE(modeOf(Libraries::storePath(e.prefix.path(), id)), 0700);
        const auto f = e.records();
        QCOMPARE(f.libraries.size(), 1);
        QCOMPARE(f.libraries[0].source.sha256, ImageSources::sha256Hex("image-bytes"));
    }
    void reimportOfIdenticalBytesIsIdempotent() {
        Env e; QVERIFY(e.import(kLib, "same")); QVERIFY(e.import(kLib, "same"));
        QCOMPARE(e.records().libraries.size(), 1);
        QCOMPARE(slurp(e.upper()), QByteArray("same"));
    }
    void identicalReimportDoesNotRewriteTheUpperFile() {
        Env e; QVERIFY(e.import(kLib, "same")); struct stat a, b;
        QVERIFY(::stat(e.upper().toUtf8().constData(), &a) == 0);
        QVERIFY(e.import(kLib, "same"));
        QVERIFY(::stat(e.upper().toUtf8().constData(), &b) == 0);
        QCOMPARE(quint64(b.st_ino), quint64(a.st_ino));
    }
    void recordedShaIsTheLauncherComputedLowercaseHash() {
        Env e; QString err; LibrarySource s = srcFor("b"); s.sha256 = s.sha256.toUpper();
        QVERIFY2(importImage(e.prefix.path(), e.runtime.path(), kLib, "b", s, false, &err, e.opts), qPrintable(err));
        QCOMPARE(e.records().libraries[0].source.sha256, ImageSources::sha256Hex("b"));
    }
    void reimportOfOurOwnFileWithNewBytesReplacesIt() {
        Env e; QVERIFY(e.import(kLib, "v1")); QVERIFY(e.import(kLib, "v2"));
        QCOMPARE(slurp(e.upper()), QByteArray("v2"));
        QCOMPARE(e.records().libraries[0].source.sha256, ImageSources::sha256Hex("v2"));
    }
    void foreignFileAtDestinationIsRefused() {
        Env e; put(e.upper(), "guest file"); QString err;
        QVERIFY(!e.import(kLib, "mine", false, &err));
        QCOMPARE(slurp(e.upper()), QByteArray("guest file"));
        QVERIFY(e.records().libraries.isEmpty());
    }
    void recordedFileThatWasTamperedWithIsRefused() {
        Env e; QVERIFY(e.import(kLib, "v1")); put(e.upper(), "tampered");
        QVERIFY(!e.import(kLib, "v2"));
        QCOMPARE(slurp(e.upper()), QByteArray("tampered"));
    }
    void symlinkAtDestinationIsRefused() {
        Env e; put(e.evil.path() + "/target", "keep");
        QDir().mkpath(e.prefix.path() + "/usr/lib");
        QVERIFY(QFile::link(e.evil.path() + "/target", e.upper()));
        QVERIFY(!e.import(kLib, "mine"));
        QCOMPARE(slurp(e.evil.path() + "/target"), QByteArray("keep"));
    }
    void pinnedPathIsRefusedUnlessForced() {
        Env e; QVERIFY(!e.import(kAppKit, "bytes")); QVERIFY(!QFileInfo::exists(e.upper(kAppKit)));
        QVERIFY(e.import(kAppKit, "bytes", true));
        QCOMPARE(slurp(e.upper(kAppKit)), QByteArray("bytes"));
    }
    void runtimeProvidedNonPinnedPathIsRefusedUnlessForced() {
        Env e; put(e.runtime.path() + kLib, "runtime copy"); QString err;
        QVERIFY(!Libraries::isPinned(kLib));
        QVERIFY(!e.import(kLib, "mine", false, &err)); QVERIFY(err.contains("runtime"));
        QVERIFY(!QFileInfo::exists(e.upper()) && !QFileInfo::exists(e.prefix.path() + "/.darling-launcher/libraries.json"));
        QVERIFY(e.import(kLib, "mine", true));
        QCOMPARE(slurp(e.upper()), QByteArray("mine"));
    }
    void runningPrefixIsRefused() {
        Env e; put(e.prefix.path() + "/.init.pid", "4242");
        QDir().mkpath(e.proc.path() + "/4242");
        QVERIFY(QFile::link("/usr/libexec/darling/bin/darlingserver", e.proc.path() + "/4242/exe"));
        QString err; QVERIFY(!e.import(kLib, "b", false, &err)); QVERIFY(err.contains("running"));
        QVERIFY(!QFileInfo::exists(e.upper()) && !QFileInfo::exists(e.prefix.path() + "/.darling-launcher/libraries.json"));
        QVERIFY(!e.import(kLib, "b", true)); // force does not override a running prefix
    }
    void liveSocketIsRefused() {
        Env e; e.opts.procFs.socketMayBeLive = [](const QString &) { return true; };
        QVERIFY(!e.import(kLib, "b")); QVERIFY(!QFileInfo::exists(e.upper()));
    }
    void lyingSourceShaIsRefused() {
        Env e; QString err;
        LibrarySource lie{"fixture", "u", "t", ImageSources::sha256Hex("other")};
        QVERIFY(!importImage(e.prefix.path(), e.runtime.path(), kLib, "b", lie, false, &err, e.opts));
        QVERIFY(err.contains("sha256")); QVERIFY(e.untouched());
    }
    void invalidSourceKindIsRefusedBeforeAnyWrite() {
        Env e; QString err; LibrarySource bad{"bogus", "u", "t", ImageSources::sha256Hex("b")};
        QVERIFY(!importImage(e.prefix.path(), e.runtime.path(), kLib, "b", bad, false, &err, e.opts));
        QVERIFY(e.untouched());
    }
    void invalidInstallNameIsRefused() {
        Env e; QVERIFY(!e.import("@rpath/X", "b")); QVERIFY(!e.import("/etc/passwd", "b")); QVERIFY(e.untouched());
    }
    void emptyBytesAreRefused() { Env e; QVERIFY(!e.import(kLib, QByteArray())); QVERIFY(e.untouched()); }
    void symlinkPlantedBeforeTheWalkIsRefused() {
        Env e; QVERIFY(QFile::link(e.evil.path(), e.prefix.path() + "/usr"));
        QVERIFY(!e.import(kLib, "b")); QVERIFY(QDir(e.evil.path()).isEmpty());
    }
    void symlinkedLauncherDirIsRefused() {
        Env e; QVERIFY(QFile::link(e.evil.path(), e.prefix.path() + "/.darling-launcher"));
        QVERIFY(!e.import(kLib, "b")); QVERIFY(QDir(e.evil.path()).isEmpty()); QVERIFY(!QFileInfo::exists(e.upper()));
    }
    void parentSwappedAfterTheChainIsOpenedStillWritesToTheOriginalDirectory() {
        Env e;
        e.opts.upperWriteHooks.afterDirsOpened = [&] {
            QVERIFY(QDir().rename(e.prefix.path() + "/usr/lib", e.prefix.path() + "/usr/moved"));
            QVERIFY(QFile::link(e.evil.path(), e.prefix.path() + "/usr/lib"));
        };
        QString err; QVERIFY2(e.import(kLib, "b", false, &err), qPrintable(err));
        QCOMPARE(slurp(e.prefix.path() + "/usr/moved/libfoo.dylib"), QByteArray("b"));
        QVERIFY(QDir(e.evil.path()).isEmpty());
    }
    // Aborts a fresh import or a replace after `stage`, then checks that nothing unrecognisable is left and that
    // re-importing completes it from that crash point.
    void crashAtEveryStepIsRecoverable_data() {
        QTest::addColumn<QString>("stage"); QTest::addColumn<bool>("replace");
        for (const char *st : {"pending", "upper", "record"}) {
            QTest::newRow(qPrintable(QString("fresh after %1").arg(st))) << QString(st) << false;
            QTest::newRow(qPrintable(QString("replace after %1").arg(st))) << QString(st) << true;
        }
    }
    void crashAtEveryStepIsRecoverable() {
        QFETCH(QString, stage); QFETCH(bool, replace);
        Env e; QString err;
        if (replace) QVERIFY(e.import(kLib, "old"));
        e.opts.abortAfter = [&](const QString &s) { return s == stage; };
        QVERIFY(!e.import(kLib, "new"));
        const auto f = e.records();
        UpperState st; QVERIFY2(upperFileState(e.prefix.path(), kLib, f.libraries.isEmpty() ? nullptr : &f.libraries[0], 1 << 20, &st, &err), qPrintable(err));
        QVERIFY2(!st.exists || st.ours, "an upper file survived that the launcher cannot recognise");
        QCOMPARE(st.exists, stage != "pending" || replace);
        if (!replace && stage == "pending") QVERIFY(!QFileInfo::exists(e.upper()));
        e.opts.abortAfter = nullptr;
        QVERIFY2(e.import(kLib, "new", false, &err), qPrintable(err));
        QCOMPARE(slurp(e.upper()), QByteArray("new"));
        const QString store = Libraries::storePath(e.prefix.path(), Libraries::idFor(kLib));
        QCOMPARE(slurp(store + "/image"), QByteArray("new"));
        QVERIFY(!QFileInfo::exists(store + "/image.pending"));
        QCOMPARE(e.records().libraries.size(), 1);
        QCOMPARE(e.records().libraries[0].source.sha256, ImageSources::sha256Hex("new"));
    }
    void liveImageIsNeverReplacedBeforeTheRecordIsWritten() {
        Env e; QVERIFY(e.import(kLib, "old"));
        e.opts.abortAfter = [](const QString &s) { return s == "upper"; };
        QVERIFY(!e.import(kLib, "new"));
        QCOMPARE(slurp(Libraries::storePath(e.prefix.path(), Libraries::idFor(kLib)) + "/image"), QByteArray("old"));
    }
    void differentBytesAfterEachCrashPointSucceedAndEndConsistent_data() {
        QTest::addColumn<QString>("firstCrash"); QTest::addColumn<QString>("secondCrash");
        for (const char *a : {"pending", "upper", "record"})
            for (const char *b : {"", "pending", "upper", "record"})
                QTest::newRow(qPrintable(QString("crash %1 then %2").arg(a, *b ? b : "no crash"))) << QString(a) << QString(b);
    }
    void differentBytesAfterEachCrashPointSucceedAndEndConsistent() {
        QFETCH(QString, firstCrash); QFETCH(QString, secondCrash);
        Env e; QString err; QVERIFY(e.import(kLib, "old"));
        e.opts.abortAfter = [&](const QString &s) { return s == firstCrash; };
        QVERIFY(!e.import(kLib, "new"));
        e.opts.abortAfter = [&](const QString &s) { return !secondCrash.isEmpty() && s == secondCrash; };
        QCOMPARE(e.import(kLib, "third", false, &err), secondCrash.isEmpty());
        if (!secondCrash.isEmpty()) { // the second import was cut short too: nothing unrecognisable may remain
            const auto f = e.records();
            UpperState st; QVERIFY(upperFileState(e.prefix.path(), kLib, &f.libraries[0], 1 << 20, &st, &err));
            QVERIFY2(!st.exists || st.ours, "unrecognisable upper file");
            e.opts.abortAfter = nullptr; QVERIFY2(e.import(kLib, "third", false, &err), qPrintable(err));
        }
        QCOMPARE(slurp(e.upper()), QByteArray("third"));
        const QString store = Libraries::storePath(e.prefix.path(), Libraries::idFor(kLib));
        QCOMPARE(slurp(store + "/image"), QByteArray("third"));
        QVERIFY(!QFileInfo::exists(store + "/image.pending"));
        QCOMPARE(e.records().libraries[0].source.sha256, ImageSources::sha256Hex("third"));
    }
    void crashRightAfterRemovingTheInterruptedFileIsRecoverable_data() {
        QTest::addColumn<QByteArray>("again");
        QTest::newRow("same bytes") << QByteArray("third"); QTest::newRow("different bytes") << QByteArray("fourth");
    }
    void crashRightAfterRemovingTheInterruptedFileIsRecoverable() {
        QFETCH(QByteArray, again);
        Env e; QString err; QVERIFY(e.import(kLib, "old"));
        e.opts.abortAfter = [](const QString &s) { return s == "upper"; };
        QVERIFY(!e.import(kLib, "new"));
        e.opts.abortAfter = [](const QString &s) { return s == "unlinked"; };
        QVERIFY(!e.import(kLib, "third")); QVERIFY(!QFileInfo::exists(e.upper()));
        e.opts.abortAfter = nullptr; QVERIFY2(e.import(kLib, again, false, &err), qPrintable(err));
        QCOMPARE(slurp(e.upper()), again);
        QCOMPARE(slurp(Libraries::storePath(e.prefix.path(), Libraries::idFor(kLib)) + "/image"), again);
        QCOMPARE(e.records().libraries[0].source.sha256, ImageSources::sha256Hex(again));
    }
    void differentBytesAfterAnInterruptedFreshImportSucceed() {
        Env e; e.opts.abortAfter = [](const QString &s) { return s == "upper"; };
        QVERIFY(!e.import(kLib, "first"));
        e.opts.abortAfter = nullptr; QString err; QVERIFY2(e.import(kLib, "second", false, &err), qPrintable(err));
        QCOMPARE(slurp(e.upper()), QByteArray("second"));
    }
    void pendingFileDoesNotMakeAForeignFileOurs() {
        Env e; put(e.upper(), "guest file");
        QVERIFY(!e.import(kLib, "mine"));
        QCOMPARE(slurp(e.upper()), QByteArray("guest file"));
    }
    void runtimeRootMustBeAnExistingAbsoluteDirectory() {
        Env e; QString err;
        QVERIFY(!importImage(e.prefix.path(), ".", kLib, "b", srcFor("b"), false, &err, e.opts)); // exists, but relative
        QVERIFY(!importImage(e.prefix.path(), "relative/dir", kLib, "b", srcFor("b"), false, &err, e.opts));
        QVERIFY(!importImage(e.prefix.path(), e.runtime.path() + "/missing", kLib, "b", srcFor("b"), false, &err, e.opts));
        put(e.runtime.path() + "/afile", "x");
        QVERIFY(!importImage(e.prefix.path(), e.runtime.path() + "/afile", kLib, "b", srcFor("b"), false, &err, e.opts));
        QVERIFY(e.untouched());
    }
    void runtimeFrameworkDirectoryProtectsEveryVersionedBinary() {
        Env e; const QString name = "/System/Library/PrivateFrameworks/Foo.framework/Versions/A/Foo";
        QDir().mkpath(e.runtime.path() + "/System/Library/PrivateFrameworks/Foo.framework");
        QString err; QVERIFY(!Libraries::isPinned(name));
        QVERIFY(!e.import(name, "b", false, &err)); QVERIFY(err.contains("runtime"));
        QVERIFY(!QFileInfo::exists(e.upper(name)));
        QVERIFY(e.import(name, "b", true));
    }
    void masterSwitchRefusesImportUnlessOverridden() {
        Env e; QString err; LibrariesFile f; f.enabled = false; QVERIFY(Libraries::save(e.prefix.path(), f, &err));
        QVERIFY(!e.import(kLib, "b", false, &err)); QVERIFY(err.contains("switched off"));
        QVERIFY(!QFileInfo::exists(e.upper()));
        e.opts.overrideMasterSwitch = true; QVERIFY(e.import(kLib, "b"));
    }
    void outgrowingLibrariesJsonIsRefusedBeforeAnythingIsWritten() {
        Env e; QString err; LibrariesFile f;
        const QString filler = (QString(240, 'p') + "/").repeated(11);
        for (const QString &stem : {QString("/usr/lib/" + filler + "big%1.dylib"), QString("/usr/lib/lib%1.dylib")}) {
            bool full = false; // fill libraries.json to its size limit, long names first, then short ones
            for (int i = 0; i < 4000 && !full; ++i) {
                const QString n = stem.arg(i);
                f.libraries.append({n, Libraries::idFor(n), true, srcFor("x")});
                QByteArray json; if (!Libraries::serialize(f, &json, &err)) { f.libraries.removeLast(); full = true; }
                if (!full && stem.contains("big") && i >= 400) break;
            }
            QVERIFY2(full, "serialize never hit the size limit");
        }
        QVERIFY2(Libraries::save(e.prefix.path(), f, &err), qPrintable(err));
        QVERIFY(!e.import("/usr/lib/" + filler + "target.dylib", "b", false, &err)); QVERIFY2(err.contains("size limit"), qPrintable(err));
        QVERIFY(!QFileInfo::exists(e.prefix.path() + "/.darling-launcher/libs"));
        QVERIFY(!QFileInfo::exists(e.upper("/usr/lib/" + filler + "target.dylib")));
    }
    void oversizedImageIsRefused() {
        Env e; e.opts.maxImageBytes = 4; QVERIFY(!e.import(kLib, "12345")); QVERIFY(e.untouched());
        QVERIFY(e.import(kLib, "1234"));
    }
    void lockIsTakenBeforeTheRunningCheck() {
        Env e; QString err; put(e.prefix.path() + "/.init.pid", "4242");
        QDir().mkpath(e.proc.path() + "/4242");
        QVERIFY(QFile::link("/x/darlingserver", e.proc.path() + "/4242/exe"));
        const int fd = SafeFs::lockFile(e.prefix.path(), ".darling-launcher/lock", 0755, &err); QVERIFY(fd >= 0);
        QVERIFY(!e.import(kLib, "b", false, &err)); QVERIFY(err.contains("Another launcher"));
        ::close(fd);
    }
    void concurrentLauncherHoldingTheLockIsRefused() {
        Env e; QString err; const int fd = SafeFs::lockFile(e.prefix.path(), ".darling-launcher/lock", 0755, &err); QVERIFY(fd >= 0);
        QVERIFY(!e.import(kLib, "b")); QVERIFY(!QFileInfo::exists(e.upper()));
        ::close(fd); QVERIFY(e.import(kLib, "b"));
    }
};
QTEST_APPLESS_MAIN(ImportTest)
#include "libraryimport_test.moc"
