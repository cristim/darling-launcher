// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic only: cache files are generated headers plus filler (dyldcache_fixture.h), apps are built by macho_fixture.h.
#include "appneedscache.h"
#include "dyldcache_fixture.h"
#include "dyldcacheconfig.h"
#include "macho_fixture.h"
#include "appneeds.h"
#include <QTemporaryDir>
#include <atomic>

using namespace AppNeedsCache;

static const QString kFw = "/System/Library/Frameworks/OnlyInCache.framework/Versions/A/OnlyInCache";
static const QString kLibz = "/usr/lib/libz.1.dylib";
static bool put(const QString &path, const QByteArray &data) {
    return QDir().mkpath(QFileInfo(path).absolutePath()) && [&] { QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(data) == data.size(); }();
}

struct Fx {
    QTemporaryDir dir;
    QString volume, cacheVolume, prefix, storeRoot;
    Options opt;
    QByteArray uuid;
    Fx() {
        volume = dir.path() + "/volume"; cacheVolume = dir.path() + "/cryptex"; prefix = dir.path() + "/prefix"; storeRoot = dir.path() + "/store/root";
        QDir().mkpath(volume); QDir().mkpath(prefix);
        uuid = makeFakeCache(cacheVolume + "/System/Library/dyld", "arm64e", 1, {".01", ".02", ".03.dylddata"}, 4096);
        for (const QString &other : {"dyld_shared_cache_arm64e.map", "dyld_shared_cache_arm64e.atlas", "aot_shared_cache.0", "dyld_shared_cache_x86_64"})
            QVERIFY(put(cacheVolume + "/System/Library/dyld/" + other, QByteArray(777, 'o')));
        opt.copy.windowBytes = 1024; opt.copy.blockBytes = 512; opt.copy.pauseMs = 0;
        opt.marginBytes = 0;
        opt.freeBytes = [](const QString &) { return qint64(1) << 40; };
    }
    void app(const QStringList &names, quint32 subtype = 2) { QVERIFY(put(volume + "/System/Applications/Fake.app/Contents/MacOS/Fake", thin(subtype, names))); }
    Request request(const QStringList &linked) const { return {"Fake", volume, cacheVolume, prefix, storeRoot, "System/Applications/Fake.app/Contents/MacOS/Fake", linked}; }
    QString srcDir() const { return cacheVolume + "/System/Library/dyld"; }
    QStringList storeDirs() const { return QDir(storeRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot); }
};
static QString slurp(const QString &p) { QFile f(p); return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString(); }

class AppNeedsCacheTest : public QObject {
    Q_OBJECT
private slots:
    void detectionNeedsArm64eAndCacheServedNames() {
        Fx fx; Preview p; QString error;
        fx.app({kFw}, 2);
        QVERIFY(buildPreview(fx.request({kFw}), fx.opt, &p, &error)); QVERIFY2(p.cacheApp, qPrintable(p.detail));
        fx.app({kFw}, 0);                                    // an arm64 (not arm64e) app never uses the cache
        QVERIFY(buildPreview(fx.request({kFw}), fx.opt, &p, &error)); QVERIFY(!p.cacheApp); QVERIFY2(p.detail.contains("arm64e"), qPrintable(p.detail));
        fx.app({kFw}, 2);
        QVERIFY(put(fx.volume + kFw, "an on-disk image"));   // every linked name has a file: nothing is cache-served
        QVERIFY(buildPreview(fx.request({kFw}), fx.opt, &p, &error)); QVERIFY(!p.cacheApp); QVERIFY2(p.detail.contains("has a file"), qPrintable(p.detail));
        QVERIFY(summarize(p).contains("not needed"));
    }
    void previewListsNamesAndSizesOfTheArm64eFilesOnly() {
        Fx fx; fx.app({kFw, kLibz}); Preview p; QString error;
        QVERIFY(buildPreview(fx.request({kFw, kLibz}), fx.opt, &p, &error));
        QStringList names; for (const CacheFile &f : p.files) names << f.name;
        QCOMPARE(names, (QStringList{"dyld_shared_cache_arm64e", "dyld_shared_cache_arm64e.01", "dyld_shared_cache_arm64e.02", "dyld_shared_cache_arm64e.03.dylddata"}));
        qint64 sum = 0; for (const CacheFile &f : p.files) sum += QFileInfo(fx.srcDir() + "/" + f.name).size();
        QCOMPARE(p.totalBytes, sum); QVERIFY(p.blocker.isEmpty()); QVERIFY(!p.alreadyPresent());
        QCOMPARE(p.servedLibraries, QStringList{kLibz});
        const QString text = summarize(p);
        for (const QString &needle : QStringList{"dyld_shared_cache_arm64e.01", "Total:", "Apple executable programs", "never committed", "licence", kLibz})
            QVERIFY2(text.contains(needle), qPrintable(needle));
        QVERIFY(!text.contains(".atlas") && !text.contains("aot_shared") && !text.contains("x86_64"));
        QVERIFY(!QFileInfo::exists(fx.storeRoot)); // a preview writes nothing
    }
    void noCopyWithoutTheNoticeBeingAccepted() {
        Fx fx; fx.app({kFw, kLibz}); Preview p; QString error; Applied a;
        QVERIFY(buildPreview(fx.request({kFw, kLibz}), fx.opt, &p, &error));
        QVERIFY(!apply(fx.request({kFw, kLibz}), p, false, fx.opt, &a, &error)); QVERIFY2(error.contains("not accepted"), qPrintable(error));
        QVERIFY(!QFileInfo::exists(fx.storeRoot)); QVERIFY(!QFileInfo::exists(fx.prefix + "/.darling-launcher"));
    }
    void acceptedCopyIsVerifiedPointsThePrefixAndIsLoggedThenIdempotent() {
        Fx fx; fx.app({kFw, kLibz}); Preview p; QString error; Applied a;
        QVERIFY(buildPreview(fx.request({kFw, kLibz}), fx.opt, &p, &error));
        QVERIFY2(apply(fx.request({kFw, kLibz}), p, true, fx.opt, &a, &error), qPrintable(error));
        QVERIFY(a.copied); QVERIFY(a.pointerWritten);
        QCOMPARE(DyldCacheStore::status(fx.storeRoot, a.storeId).state, StoreState::Ready);
        for (const CacheFile &f : p.files) QCOMPARE(QFileInfo(fx.storeRoot + "/" + a.storeId + "/" + f.name).size(), f.size);
        DyldCacheConfig config; QVERIFY(DyldCacheConfigFile::load(fx.prefix, &config, &error));
        QCOMPARE(config.store, a.storeId); QCOMPARE(config.served, QStringList{kLibz}); QVERIFY(DyldCacheConfigFile::wantsCache(config));
        const QString log = slurp(AppNeeds::logPath(fx.prefix));
        QVERIFY(log.contains("| Fake | dyld cache | 4 files |")); QVERIFY(log.contains("copied"));
        // second run: found in the store by names and sizes, skipped, one more log row, nothing copied again
        Preview again; QVERIFY(buildPreview(fx.request({kFw, kLibz}), fx.opt, &again, &error));
        QVERIFY(again.alreadyPresent()); QVERIFY(summarize(again).contains("Already present and identical"));
        Applied b; QVERIFY2(apply(fx.request({kFw, kLibz}), again, true, fx.opt, &b, &error), qPrintable(error));
        QVERIFY(!b.copied); QCOMPARE(b.storeId, a.storeId); QCOMPARE(fx.storeDirs().size(), 1);
        QCOMPARE(slurp(AppNeeds::logPath(fx.prefix)).count("| dyld cache |"), 2);
    }
    void appsThatLinkNoValidatedLibraryLeaveThePrefixSettingsAlone() {
        Fx fx; fx.app({kFw}); Preview p; QString error; Applied a;
        QVERIFY(buildPreview(fx.request({kFw}), fx.opt, &p, &error)); QVERIFY(p.servedLibraries.isEmpty());
        QVERIFY(summarize(p).contains("settings stay unchanged"));
        QVERIFY2(apply(fx.request({kFw}), p, true, fx.opt, &a, &error), qPrintable(error));
        QVERIFY(a.copied); QVERIFY(!a.pointerWritten); QVERIFY(!QFileInfo::exists(DyldCacheConfigFile::configPath(fx.prefix)));
    }
    void aStoreThatDoesNotFitIsRefusedInThePreviewAndTheApply() {
        Fx fx; fx.app({kFw, kLibz}); Preview p; QString error; Applied a;
        fx.opt.freeBytes = [](const QString &) { return qint64(4096); };
        QVERIFY(buildPreview(fx.request({kFw, kLibz}), fx.opt, &p, &error));
        QVERIFY2(p.blocker.contains("Not enough free space"), qPrintable(p.blocker)); QVERIFY(summarize(p).contains("Cannot be offered"));
        QVERIFY(!apply(fx.request({kFw, kLibz}), p, true, fx.opt, &a, &error)); QVERIFY(!QFileInfo::exists(fx.storeRoot));
        fx.opt.freeBytes = [](const QString &) { return qint64(-1); };
        QVERIFY(buildPreview(fx.request({kFw, kLibz}), fx.opt, &p, &error)); QVERIFY(p.blocker.contains("free space"));
    }
    void aCancelledCopyLeavesNothingBehind() {
        Fx fx; fx.app({kFw, kLibz}); Preview p; QString error; Applied a;
        QVERIFY(buildPreview(fx.request({kFw, kLibz}), fx.opt, &p, &error));
        std::atomic<bool> cancel{false};
        const auto r = apply(fx.request({kFw, kLibz}), p, true, fx.opt, &a, &error, [&](const DyldCacheCopy::Progress &g) { if (g.totalBytes > 1000) cancel = true; }, &cancel);
        QVERIFY(!r); QVERIFY2(error.contains("cancelled") && error.contains("removed"), qPrintable(error));
        QVERIFY(fx.storeDirs().isEmpty()); QVERIFY(!QFileInfo::exists(AppNeeds::logPath(fx.prefix)));
    }
    void aCorruptedCopyIsDetectedAndRemoved() {
        Fx fx; fx.app({kFw, kLibz}); Preview p; QString error; Applied a;
        QVERIFY(buildPreview(fx.request({kFw, kLibz}), fx.opt, &p, &error));
        // the source changes under the copy: its header no longer carries the UUID that was read first
        fx.opt.copy.onWindow = [&](const QString &file, qint64 offset, qint64) {
            if (file.endsWith(".02") && offset == 0) { QFile f(fx.srcDir() + "/" + file); QVERIFY(f.open(QIODevice::ReadWrite)); f.seek(0x58); f.write(QByteArray(16, 'Z')); }
        };
        QVERIFY(!apply(fx.request({kFw, kLibz}), p, true, fx.opt, &a, &error)); QVERIFY2(error.contains("UUID") || error.contains("changed"), qPrintable(error));
        QVERIFY(fx.storeDirs().isEmpty()); QVERIFY(!QFileInfo::exists(DyldCacheConfigFile::configPath(fx.prefix)));
    }
    void aStoreThatWasAlreadyPartialKeepsItsResumableState() {
        Fx fx; fx.app({kFw, kLibz}); Preview p; QString error; Applied a;
        std::atomic<bool> cancel{false}; DyldCacheCopy::Options engine = fx.opt.copy; engine.marginBytes = 0; engine.freeBytes = fx.opt.freeBytes;
        const auto first = DyldCacheCopy::copyCache(fx.srcDir(), "arm64e", fx.storeRoot, engine, [&](const DyldCacheCopy::Progress &g) { if (g.totalBytes > 5000) cancel = true; }, &cancel);
        QCOMPARE(int(first.result), int(DyldCacheCopy::Result::Cancelled)); QCOMPARE(fx.storeDirs().size(), 1);
        QVERIFY(buildPreview(fx.request({kFw, kLibz}), fx.opt, &p, &error));
        cancel = false;
        QVERIFY(!apply(fx.request({kFw, kLibz}), p, true, fx.opt, &a, &error, [&](const DyldCacheCopy::Progress &) { cancel = true; }, &cancel));
        QCOMPARE(fx.storeDirs().size(), 1); // not this call's to delete
    }
    void aSymlinkedMainFileOrDyldDirectoryIsRefused() {
        Fx fx; fx.app({kFw, kLibz}); Preview p; QString error;
        const QString main = fx.srcDir() + "/dyld_shared_cache_arm64e";
        QVERIFY(QFile::rename(main, fx.dir.path() + "/elsewhere")); QVERIFY(QFile::link(fx.dir.path() + "/elsewhere", main));
        QVERIFY(buildPreview(fx.request({kFw, kLibz}), fx.opt, &p, &error)); QVERIFY2(p.blocker.contains("no dyld_shared_cache_arm64e"), qPrintable(p.blocker));
        Fx other; other.app({kFw});
        QVERIFY(QDir(other.cacheVolume + "/System/Library").rename("dyld", "real-dyld"));
        QVERIFY(QFile::link(other.dir.path(), other.cacheVolume + "/System/Library/dyld")); // leads out of the volume
        QVERIFY(buildPreview(other.request({kFw}), other.opt, &p, &error)); QVERIFY2(!p.blocker.isEmpty(), "a dyld directory that leaves the volume must block");
        Applied a; QVERIFY(!apply(other.request({kFw}), p, true, other.opt, &a, &error)); QVERIFY(!QFileInfo::exists(other.storeRoot));
    }
    void noCacheVolumeBlocksAndTheFinderFindsTheMount() {
        Fx fx; fx.app({kFw}); Preview p; QString error;
        Request r = fx.request({kFw}); r.cacheVolume.clear();
        QVERIFY(buildPreview(r, fx.opt, &p, &error)); QVERIFY2(p.blocker.contains("not mounted"), qPrintable(p.blocker));
        QCOMPARE(findCacheVolume(fx.volume, {}), QString());
        QCOMPARE(findCacheVolume(fx.volume, {SourceMount{fx.cacheVolume, "dev", "fuse", true}}), fx.cacheVolume);
        QCOMPARE(findCacheVolume(fx.volume, {SourceMount{fx.dir.path(), "dev", "fuse", true}}), QString()); // not a volume that holds System/Library/dyld
    }
    void thePrefixThatUsesAnotherStoreIsNotRepointed() {
        Fx fx; fx.app({kFw, kLibz}); Preview p; QString error; Applied a;
        const QString other = makeFakeStore(fx.storeRoot, "arm64e", 9, {".01"}, 4096);
        const auto owner = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
        QVERIFY(QFile::setPermissions(fx.storeRoot, owner)); QVERIFY(QFile::setPermissions(fx.storeRoot + "/" + other, owner));
        DyldCacheConfig c; c.mode = DyldCacheMode::AllowList; c.store = other; c.served = {kLibz};
        QVERIFY(DyldCacheConfigFile::save(fx.prefix, c, &error));
        QVERIFY(buildPreview(fx.request({kFw, kLibz}), fx.opt, &p, &error));
        QVERIFY2(apply(fx.request({kFw, kLibz}), p, true, fx.opt, &a, &error), qPrintable(error));
        QVERIFY(!a.pointerWritten); QVERIFY(a.pointerNote.contains("another cache store"));
        DyldCacheConfig now; QVERIFY(DyldCacheConfigFile::load(fx.prefix, &now, &error)); QCOMPARE(now.store, other);
    }
    void theNoticeSaysWhatTheUserNeedsToKnow() {
        const QString text = notice(5LL << 30);
        for (const QString &needle : {"Apple executable programs", "bit for bit", "this computer", "private folder", "stay on this machine", "never committed, uploaded or redistributed", "licence"})
            QVERIFY2(text.contains(needle), qPrintable(needle));
        QVERIFY(text.contains("5.0 GiB"));
    }
};
QTEST_GUILESS_MAIN(AppNeedsCacheTest)
#include "appneedscache_test.moc"
