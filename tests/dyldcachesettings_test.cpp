// SPDX-License-Identifier: GPL-3.0-or-later
#include "dyldcache_fixture.h"
#include "dyldcachesettings.h"
#include "macho_fixture.h"
#include <QTemporaryDir>
#include <sys/stat.h>

using namespace DyldCacheSettings;

static const QString kLibz = "/usr/lib/libz.1.dylib";
static QString idOfSeed(int seed) { return DyldCacheStore::storeId("arm64e", DyldCache::uuidHex(fakeUuid(seed * 100))); }

class DyldCacheSettingsTest : public QObject {
    Q_OBJECT
private slots:
    void sourceDirIsTheVolumesDyldDirectoryAndStaysInsideTheVolume() {
        QTemporaryDir vol, outside; QString dir, err;
        QVERIFY(!resolveSourceDir(QString(), &dir, &err)); QVERIFY(err.contains("No mounted"));
        QVERIFY(!resolveSourceDir(vol.path() + "/absent", &dir, &err)); QVERIFY(err.contains("not a mounted directory"));
        QVERIFY(!resolveSourceDir(vol.path(), &dir, &err)); QVERIFY(err.contains("no System/Library/dyld"));
        QVERIFY(QDir().mkpath(vol.path() + "/System/Library/dyld"));
        QVERIFY2(resolveSourceDir(vol.path(), &dir, &err), qPrintable(err));
        QCOMPARE(dir, QFileInfo(vol.path()).canonicalFilePath() + "/System/Library/dyld");
        QVERIFY(!err.contains(vol.path()) || err.isEmpty());
        // a symlinked volume path resolves to the real directory
        QTemporaryDir links; QVERIFY(QFile::link(vol.path(), links.path() + "/v"));
        QVERIFY(resolveSourceDir(links.path() + "/v", &dir, &err)); QCOMPARE(dir, QFileInfo(vol.path()).canonicalFilePath() + "/System/Library/dyld");
        // System/Library/dyld pointing out of the volume is refused
        QTemporaryDir evil; QVERIFY(QDir().mkpath(evil.path() + "/System/Library"));
        QVERIFY(QFile::link(outside.path(), evil.path() + "/System/Library/dyld"));
        QVERIFY(!resolveSourceDir(evil.path(), &dir, &err)); QVERIFY(err.contains("outside the volume"));
    }
    void overviewWithoutAStoreSaysSo() {
        QTemporaryDir stores;
        auto o = overview(stores.path(), idOfSeed(1), {});
        QVERIFY(o.rows.isEmpty()); QVERIFY(o.stale); QVERIFY(o.headline.startsWith("No private store yet"));
        o = overview(stores.path(), {}, "The source cache is for arm64, not arm64e");
        QVERIFY(!o.sourceKnown); QVERIFY(!o.stale); QVERIFY(o.headline.contains("could not be compared")); QVERIFY(o.headline.contains("arm64, not arm64e"));
    }
    void aStoreOfTheSourcesCacheIsCurrentAndAnotherBuildIsStale() {
        QTemporaryDir stores;
        const QString id = makeFakeStore(stores.path(), "arm64e", 1, {".01"});
        auto o = overview(stores.path(), id, {});
        QCOMPARE(o.readyIds, QStringList{id}); QVERIFY(!o.stale); QVERIFY(o.headline.contains("current"));
        QVERIFY(o.rows[0].text.contains("Ready")); QVERIFY(o.rows[0].text.contains("2 files")); QVERIFY(o.rows[0].text.contains("2026-01-01"));
        QVERIFY(!o.rows[0].text.contains(stores.path()));
        o = overview(stores.path(), idOfSeed(2), {});             // the volume holds another build
        QVERIFY(o.stale); QVERIFY(o.headline.startsWith("STALE")); QVERIFY(o.headline.contains("different cache"));
    }
    void anUnfinishedOrDamagedStoreIsNeverCurrent() {
        QTemporaryDir stores;
        const QString id = makeFakeStore(stores.path(), "arm64e", 1, {});
        QVERIFY(QFile::remove(stores.path() + "/" + id + "/store.json"));          // partial
        auto o = overview(stores.path(), id, {});
        QVERIFY(o.stale); QVERIFY(o.readyIds.isEmpty()); QVERIFY(o.headline.contains("not finished")); QVERIFY(o.rows[0].text.contains("Incomplete"));
        QTemporaryDir stores2;
        const QString id2 = makeFakeStore(stores2.path(), "arm64e", 1, {});
        QFile f(stores2.path() + "/" + id2 + "/" + DyldCache::mainFileName("arm64e"));
        QVERIFY(::chmod(f.fileName().toUtf8().constData(), 0600) == 0); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate)); f.write("short"); f.close();
        o = overview(stores2.path(), id2, {});                                         // damaged
        QVERIFY(o.stale); QVERIFY(o.rows[0].state == StoreState::Corrupt); QVERIFY(o.rows[0].text.contains("Damaged"));
    }
    void aDamagedOtherStoreIsFlaggedBesideACurrentOne() {
        QTemporaryDir stores;
        const QString good = makeFakeStore(stores.path(), "arm64e", 1, {});
        const QString bad = makeFakeStore(stores.path(), "arm64e", 2, {});
        QFile f(stores.path() + "/" + bad + "/store.json");
        QVERIFY(::chmod(f.fileName().toUtf8().constData(), 0600) == 0); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate)); f.write("{"); f.close();
        const auto o = overview(stores.path(), good, {});
        QVERIFY(!o.stale); QVERIFY(o.headline.contains("damaged"));
    }
    void enableBlockerNamesEachReasonAndAReadyStoreHasNone() {
        QTemporaryDir stores;
        QVERIFY(enableBlocker(stores.path(), {}).contains("No ready store"));
        QVERIFY(enableBlocker(stores.path(), idOfSeed(1)).contains("not ready"));       // missing
        const QString id = makeFakeStore(stores.path(), "arm64e", 1, {});
        QCOMPARE(enableBlocker(stores.path(), id), QString());
        QVERIFY(QFile::remove(stores.path() + "/" + id + "/store.json"));
        QVERIFY(enableBlocker(stores.path(), id).contains("incomplete"));
        const QString id2 = makeFakeStore(stores.path(), "arm64e", 2, {});
        QVERIFY(::chmod(stores.path().toUtf8().constData(), 0755) == 0);                  // open to others
        QVERIFY(enableBlocker(stores.path(), id2).contains("cannot be trusted"));
        QVERIFY(!enableBlocker(stores.path(), id2).contains(stores.path()));
    }
    void prefixStatusSaysExactlyWhatIsWrong() {
        QTemporaryDir stores;
        const QString id = makeFakeStore(stores.path(), "arm64e", 1, {});
        DyldCacheConfig c;
        QVERIFY(prefixStatus(c, stores.path()).text.startsWith("Off:")); QVERIFY(!prefixStatus(c, stores.path()).effective);
        c.mode = DyldCacheMode::AllowList; c.store = id;
        auto s = prefixStatus(c, stores.path()); QVERIFY(!s.effective); QVERIFY(s.text.contains("no library is selected"));
        c.served = {kLibz};
        s = prefixStatus(c, stores.path()); QVERIFY(s.effective); QVERIFY(s.text.startsWith("Active: 1 library"));
        c.store = idOfSeed(9);
        s = prefixStatus(c, stores.path()); QVERIFY(!s.effective); QVERIFY(s.text.contains("not ready")); QVERIFY(s.text.contains("refuse to start"));
        c.mode = DyldCacheMode::Off; c.store = id;                                           // the selection is kept while off
        QVERIFY(prefixStatus(c, stores.path()).text.startsWith("Off:"));
    }
    void buildConfigKeepsTheSelectionWhileOffAndRejectsWhatIsNotValidated() {
        DyldCacheConfig prev, out; QString err;
        prev.store = "arm64e-" + QString(32, 'a');
        QVERIFY2(buildConfig(prev, true, {}, {kLibz}, &out, &err), qPrintable(err));
        QVERIFY(out.mode == DyldCacheMode::AllowList); QCOMPARE(out.store, prev.store); QCOMPARE(out.served, QStringList{kLibz});
        QVERIFY2(buildConfig(out, false, {}, {kLibz}, &out, &err), qPrintable(err));
        QVERIFY(out.mode == DyldCacheMode::Off); QCOMPARE(out.served, QStringList{kLibz});
        QVERIFY(!buildConfig(DyldCacheConfig{}, true, {}, {kLibz}, &out, &err)); QVERIFY(err.contains("no store is chosen"));
        // a name outside the validated list is dropped, never written
        QVERIFY(buildConfig(prev, true, {}, {"/usr/lib/libevil.dylib", kLibz}, &out, &err)); QCOMPARE(out.served, QStringList{kLibz});
        QVERIFY(buildConfig(prev, true, {}, {}, &out, &err)); QVERIFY(out.served.isEmpty());
        QVERIFY(!buildConfig(prev, true, "not-a-store-id", {}, &out, &err));
    }
    void appLineAppliesTheLaunchRule() {
        QTemporaryDir prefix; QVERIFY(QDir().mkpath(prefix.path() + "/bin"));
        const auto put = [&](const char *name, const QByteArray &data) { QFile f(prefix.path() + "/bin/" + name); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(data); };
        put("e", thin(2, {})); put("p", thin(0, {}));
        DyldCacheConfig on; on.mode = DyldCacheMode::AllowList; on.store = idOfSeed(1); on.served = {kLibz};
        QVERIFY(appLine(prefix.path(), "E", "bin/e", on).contains("gets the cache"));
        QVERIFY(appLine(prefix.path(), "P", "bin/p", on).contains("non-arm64e"));
        DyldCacheConfig off = on; off.mode = DyldCacheMode::Off;
        QVERIFY(appLine(prefix.path(), "E", "bin/e", off).contains("not turned on"));
        QVERIFY(!appLine(prefix.path(), "P", "bin/p", on).contains(prefix.path()));
        QVERIFY(launchRuleHelp().contains("exactly one arm64 slice")); QVERIFY(launchRuleHelp().contains("maintenance runs"));
    }
    void progressAndSizeText() {
        DyldCacheCopy::Progress p; p.fileIndex = 4; p.fileCount = 13; p.totalBytes = (qint64(21) << 30) / 10; p.totalSize = (qint64(58) << 30) / 10;
        QCOMPARE(progressText(p), QString("file 4 of 13, 2.1 GiB of 5.8 GiB"));
        QCOMPARE(formatBytes(512 << 10), QString("0.5 MiB"));
    }
};
QTEST_APPLESS_MAIN(DyldCacheSettingsTest)
#include "dyldcachesettings_test.moc"
