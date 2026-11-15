// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic volume, synthetic cache files, a fake mount step: no Apple data, no pkexec, no mount.
#include "appneedscli.h"
#include "dyldcache_fixture.h"
#include "macho_fixture.h"
#include "core.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

static const QString kApp = "System/Applications/Fake.app";
static const QString kUi = "/System/Library/PrivateFrameworks/FakeUI.framework/Versions/A/FakeUI";
static bool put(const QString &path, const QByteArray &data) {
    return QDir().mkpath(QFileInfo(path).absolutePath()) && [&] { QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(data) == data.size(); }();
}
static QStringList tree(const QString &root) {
    QStringList all; QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) { it.next(); all << QDir(root).relativeFilePath(it.filePath()); }
    all.sort(); return all;
}
struct Run { int code; QString out, err; };

struct FakeStep : AutoMount::Step {
    QList<AutoMount::StepRequest> asked; std::function<void()> effect;
    AutoMount::StepResult run(const AutoMount::StepRequest &r) override { asked << r; if (effect) effect(); return {0, false, {}}; }
};

struct Fx {
    QTemporaryDir dir;
    QString volume, cacheVolume, prefix, store;
    QList<SourceMount> mounts;
    FakeStep step;
    QStringList answers; QList<MacPartition> partitions{{"/dev/nvme0n1p2", "apfs", "", false, {}, 245000000000LL}};
    bool interactive = true;
    Fx() {
        volume = dir.path() + "/volume"; cacheVolume = dir.path() + "/cryptex"; prefix = dir.path() + "/prefix"; store = dir.path() + "/store/root";
        QDir().mkpath(prefix);
        QVERIFY(put(volume + "/" + kApp + "/Contents/Info.plist", "<?xml version=\"1.0\" encoding=\"UTF-8\"?><plist version=\"1.0\"><dict><key>CFBundleExecutable</key><string>Fake</string></dict></plist>"));
        QVERIFY(put(volume + "/" + kApp + "/Contents/MacOS/Fake", thin(2, {kUi, "/System/Library/Frameworks/Shared.framework/Versions/A/Shared"})));
        for (const QString &fw : {"PrivateFrameworks/FakeUI", "Frameworks/Shared"}) {
            const QString base = volume + "/System/Library/" + fw + ".framework/Versions/A/Resources";
            QVERIFY(put(base + "/Info.plist", "<plist/>")); QVERIFY(put(base + "/data.bin", QByteArray(100, 'd')));
        }
        makeFakeCache(cacheVolume + "/System/Library/dyld", "arm64e", 3, {".01", ".02"}, 4096);
    }
    Run run(QStringList args, qint64 freeBytes = -1) {
        QString o, e; int code;
        { QTextStream out(&o), err(&e);
          AppImportEnv env; env.mountStep = &step; env.storeRoot = store; env.freeBytesOverride = freeBytes;
          env.interactive = [this] { return interactive; };
          env.readLine = [this] { return answers.isEmpty() ? QString() : answers.takeFirst(); };
          env.mounts = [this] { return mounts; };
          env.partitions = [this](QString *) { return partitions; };
          env.sleep = [](int) {};
          code = appImportMain(args, out, err, env); }
        return {code, o, e};
    }
    QStringList base(const QStringList &more) const { return QStringList{"--volume", volume, "--prefix", prefix, "--app", kApp} + more; }
};

class AppNeedsCliTest : public QObject {
    Q_OBJECT
private slots:
    void usageErrorsExitTwoAndCopyNothing() {
        Fx fx;
        for (const QStringList &bad : {QStringList{"--app", kApp}, fx.base({"--dry-run", "--yes"}), fx.base({"--accept-apple-binaries", "--yes"}), fx.base({"--with-cache"}), fx.base({"--yes", "--yes"}),
                                       fx.base({"--also", "", "--yes"}), fx.base({"--bogus"})}) {
            const Run r = fx.run(bad); QCOMPARE(r.code, 2); QVERIFY(r.err.contains("usage:"));
        }
        QVERIFY(tree(fx.prefix).isEmpty());
        QString text; QTextStream out(&text), err(&text);
        QCOMPARE(appImportMain({"--help"}, out, err), 0); QVERIFY(text.contains("--accept-apple-binaries"));
    }
    void dryRunAndNoFlagPrintThePlanAndCopyNothing() {
        Fx fx; const QStringList before = tree(fx.prefix);
        Run r = fx.run(fx.base({"--dry-run"}));
        QCOMPARE(r.code, 0); QVERIFY2(r.out.contains("Selected framework resources: 1") && r.out.contains("FakeUI") && r.out.contains("Optional (linked, not selected): 1") && r.out.contains("Dry run"), qPrintable(r.out));
        QVERIFY(r.out.contains("dyld shared cache:")); QCOMPARE(tree(fx.prefix), before);
        r = fx.run(fx.base({}));
        QCOMPARE(r.code, 2); QVERIFY(r.err.contains("Nothing was copied")); QCOMPARE(tree(fx.prefix), before);
    }
    void yesImportsTheAppAndOnlyTheSelectedResourcesAndIsIdempotent() {
        Fx fx;
        Run r = fx.run(fx.base({"--yes"}));
        QCOMPARE(r.code, 0); QVERIFY2(r.out.contains("Imported the app") && r.out.contains("Framework resources: 1 bundles"), qPrintable(r.out + r.err));
        QVERIFY(QFile::exists(fx.prefix + "/Applications/Fake.app/Contents/MacOS/Fake"));
        QVERIFY(QFile::exists(fx.prefix + "/System/Library/PrivateFrameworks/FakeUI.framework/Versions/A/Resources/data.bin"));
        QVERIFY(!QFileInfo::exists(fx.prefix + "/System/Library/Frameworks/Shared.framework"));   // optional: not without --also
        QVERIFY(!QFileInfo::exists(fx.store));                                                      // --yes alone never copies the cache
        QVERIFY(r.out.contains("re-run with --with-cache"));
        QVERIFY(QFile::exists(LauncherCore::catalogPath(fx.prefix)));
        const QStringList after = tree(fx.prefix);
        r = fx.run(fx.base({"--yes"}));
        QCOMPARE(r.code, 0); QVERIFY(r.out.contains("already in the prefix")); QCOMPARE(tree(fx.prefix), after);
        QVERIFY(QFile(fx.prefix + "/.darling-launcher/app-imports.md").exists());
    }
    void alsoImportsAChosenOptionalFramework() {
        Fx fx; const Run r = fx.run(fx.base({"--yes", "--also", "Shared"}));
        QCOMPARE(r.code, 0); QVERIFY2(QFile::exists(fx.prefix + "/System/Library/Frameworks/Shared.framework/Versions/A/Resources/data.bin"), qPrintable(r.err));
        QCOMPARE(fx.run(fx.base({"--yes", "--also", "NotLinked"})).code, 1);
    }
    void aShortDiskRefusesBeforeTheAppIsCopied() {
        Fx fx; const Run r = fx.run(fx.base({"--yes"}), 1 << 20);
        QCOMPARE(r.code, 1); QVERIFY2(r.err.contains("Not enough free space"), qPrintable(r.err)); QVERIFY(tree(fx.prefix).isEmpty());
    }
    void theCacheNeedsItsOwnFlagAndItsOwnAcceptance() {
        Fx fx;
        Run r = fx.run(fx.base({"--yes", "--with-cache", "--cache-volume", fx.cacheVolume}));
        QCOMPARE(r.code, 2); QVERIFY2(r.err.contains("Apple executable programs") && r.err.contains("--accept-apple-binaries"), qPrintable(r.err));
        QVERIFY(tree(fx.prefix).isEmpty()); QVERIFY(!QFileInfo::exists(fx.store));          // refused before the resources too
        r = fx.run(fx.base({"--dry-run", "--with-cache", "--cache-volume", fx.cacheVolume}));
        QCOMPARE(r.code, 0); QVERIFY(r.out.contains("dyld_shared_cache_arm64e.01")); QVERIFY(!QFileInfo::exists(fx.store)); QVERIFY(tree(fx.prefix).isEmpty());
        r = fx.run(fx.base({"--yes", "--with-cache", "--accept-apple-binaries", "--cache-volume", fx.cacheVolume}));
        QCOMPARE(r.code, 0); QVERIFY2(r.out.contains("Copied the dyld cache"), qPrintable(r.out + r.err));
        QCOMPARE(QDir(fx.store).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 1);
        r = fx.run(fx.base({"--yes", "--with-cache", "--accept-apple-binaries", "--cache-volume", fx.cacheVolume}));
        QCOMPARE(r.code, 0); QVERIFY2(r.out.contains("already present"), qPrintable(r.out)); QCOMPARE(QDir(fx.store).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 1);
    }
    void withoutAVolumeTheMountFlowIsExplainedFirstAndRefusedWithoutATerminal() {
        Fx fx; fx.interactive = false;
        const QStringList args{"--prefix", fx.prefix, "--app", kApp, "--dry-run"};
        Run r = fx.run(args);
        QCOMPARE(r.code, 1); QVERIFY(r.err.contains("not mounted") && r.err.contains("apfs-fuse")); QVERIFY(fx.step.asked.isEmpty());
        fx.interactive = true; fx.answers = {"n"};
        r = fx.run(args);
        QCOMPARE(r.code, 1); QVERIFY(r.out.contains("administrator rights") && r.out.contains("Continue? [y/N]")); QVERIFY(r.err.contains("Nothing was mounted")); QVERIFY(fx.step.asked.isEmpty());
        fx.answers = {"y"}; fx.step.effect = [&] { fx.mounts << SourceMount{fx.volume, "dev", "apfs", true}; };
        r = fx.run(args);
        QCOMPARE(r.code, 0); QCOMPARE(fx.step.asked.size(), 1); QCOMPARE(fx.step.asked[0].device, QString("/dev/nvme0n1p2")); QVERIFY(r.out.contains("Dry run"));
        QVERIFY(tree(fx.prefix).isEmpty());
    }
    void severalPartitionsAskWhichOne() {
        Fx fx; fx.partitions << MacPartition{"/dev/nvme0n1p5", "apfs", "Other", false, {}, 1000000000LL};
        fx.answers = {"2", "yes"}; fx.step.effect = [&] { fx.mounts << SourceMount{fx.volume, "dev", "apfs", true}; };
        const Run r = fx.run({"--prefix", fx.prefix, "--app", kApp, "--dry-run"});
        QCOMPARE(r.code, 0); QVERIFY(r.out.contains("1) /dev/nvme0n1p2") && r.out.contains("2) /dev/nvme0n1p5")); QCOMPARE(fx.step.asked[0].device, QString("/dev/nvme0n1p5"));
        Fx bad; bad.partitions << MacPartition{"/dev/nvme0n1p5", "apfs", "", false, {}, 1}; bad.answers = {"9"};
        QCOMPARE(bad.run({"--prefix", bad.prefix, "--app", kApp, "--dry-run"}).code, 1); QVERIFY(bad.step.asked.isEmpty());
    }
};
QTEST_GUILESS_MAIN(AppNeedsCliTest)
#include "appneedscli_test.moc"
