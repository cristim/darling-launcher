// SPDX-License-Identifier: GPL-3.0-or-later
#include "dyldcache_fixture.h"
#include "dyldcachecopy.h"
#include "safefs.h"
#include <QTemporaryDir>
#include <sys/stat.h>
#include <unistd.h>

using namespace DyldCacheCopy;

static QByteArray slurp(const QString &p) { QFile f(p); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }
static int modeOf(const QString &p) { struct stat st; return ::stat(p.toUtf8().constData(), &st) == 0 ? int(st.st_mode & 0777) : -1; }
static const QStringList kSubs = {".01", ".02", ".03"};
static const QString kMain = "dyld_shared_cache_arm64e";

struct Fx {
    QTemporaryDir src, store;
    QByteArray uuid;
    QString id;
    Options opt;
    Fx() {
        uuid = makeFakeCache(src.path(), "arm64e", 1, kSubs, 4096);
        id = DyldCacheStore::storeId("arm64e", DyldCache::uuidHex(uuid));
        opt.windowBytes = 1024; opt.blockBytes = 512; opt.marginBytes = 0;
        opt.freeBytes = [](const QString &) { return qint64(1) << 40; };
    }
    QString root() const { return store.path() + "/root"; }
    QString dst(const QString &name) const { return root() + "/" + id + "/" + name; }
    QStringList names() const { QStringList n{kMain}; for (const auto &s : kSubs) n << kMain + s; return n; }
    Outcome run(const std::atomic<bool> *cancel = nullptr, const ProgressFn &p = {}) const { return copyCache(src.path(), "arm64e", root(), opt, p, cancel); }
};

class DyldCacheCopyTest : public QObject {
    Q_OBJECT
private slots:
    void copiesEveryFileAndWritesAVerifiedStore() {
        Fx fx; QList<Progress> seen;
        const auto r = fx.run(nullptr, [&](const Progress &p) { seen << p; });
        QVERIFY2(r.result == Result::Done, qPrintable(r.error)); QCOMPARE(r.storeId, fx.id);
        for (const QString &n : fx.names()) { QCOMPARE(slurp(fx.dst(n)), slurp(fx.src.path() + "/" + n)); QCOMPARE(modeOf(fx.dst(n)), 0400); }
        QCOMPARE(modeOf(fx.root()), 0700); QCOMPARE(modeOf(fx.root() + "/" + fx.id), 0700);
        QVERIFY(!QFileInfo::exists(fx.dst(kMain + ".partial")));
        QCOMPARE(DyldCacheStore::status(fx.root(), fx.id).state, StoreState::Ready);
        QVERIFY(!seen.isEmpty()); QCOMPARE(seen.last().totalBytes, seen.last().totalSize); QCOMPARE(seen.last().totalSize, qint64(4 * 4096));
        qint64 last = 0; for (const auto &p : seen) { QVERIFY(p.totalBytes >= last); last = p.totalBytes; }
        const auto v = verifyStore(fx.root(), fx.id); QVERIFY2(v.result == Result::Done, qPrintable(v.error));
    }
    void readsInWindowsAndBlocksWithPauses() {
        Fx fx; fx.opt.pauseMs = 5; int pauses = 0; QList<qint64> lens; qint64 maxStep = 0, prev = 0; QString prevFile;
        fx.opt.sleepFn = [&](int ms) { QCOMPARE(ms, 5); ++pauses; };
        fx.opt.onWindow = [&](const QString &, qint64, qint64 len) { lens << len; };
        const auto r = fx.run(nullptr, [&](const Progress &p) { if (p.fileName == prevFile) maxStep = qMax(maxStep, p.fileBytes - prev); prev = p.fileBytes; prevFile = p.fileName; });
        QVERIFY2(r.result == Result::Done, qPrintable(r.error));
        QCOMPARE(lens.size(), 16); for (qint64 l : lens) QCOMPARE(l, qint64(1024));
        QCOMPARE(pauses, 12);          // between windows, not after the last one of a file
        QVERIFY(maxStep <= 512);
    }
    void cancelKeepsThePartialAndResumeContinuesFromAWindow() {
        Fx fx; std::atomic<bool> cancel(false); int blocks = 0;
        auto r = fx.run(&cancel, [&](const Progress &p) { if (p.fileName == kMain + ".02" && p.fileBytes == 2560 && ++blocks == 1) cancel = true; });
        QCOMPARE(r.result, Result::Cancelled);
        QVERIFY(!QFileInfo::exists(fx.dst(kMain + ".02"))); QVERIFY(QFileInfo::exists(fx.dst(kMain + ".02.partial")));
        QVERIFY(!QFileInfo::exists(fx.dst("store.json")));
        QVERIFY(QFileInfo::exists(fx.dst(kMain)));  // finished files stay
        QList<qint64> offsets; fx.opt.onWindow = [&](const QString &f, qint64 off, qint64) { if (f == kMain + ".02") offsets << off; };
        r = fx.run(); QVERIFY2(r.result == Result::Done, qPrintable(r.error));
        QVERIFY(!offsets.isEmpty()); QCOMPARE(offsets.first(), qint64(2048)); // 2560 bytes were on disk: the last window is read again
        for (const QString &n : fx.names()) QCOMPARE(slurp(fx.dst(n)), slurp(fx.src.path() + "/" + n));
    }
    void resumeRestartsWhenThePartialIsForeignOrTooLong() {
        for (const int variant : {0, 1}) {
            Fx fx; QDir().mkpath(fx.root() + "/" + fx.id); QVERIFY(QFile::setPermissions(fx.root(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
            QTemporaryDir other; makeFakeCache(other.path(), "arm64e", 9, kSubs, 4096);
            QByteArray junk = slurp(other.path() + "/" + kMain + ".01"); if (variant == 1) junk = slurp(fx.src.path() + "/" + kMain + ".01") + QByteArray(512, 'z'); // right header, too long
            { QFile f(fx.dst(kMain + ".01.partial")); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(variant == 0 ? junk.left(3000) : junk); f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner); }
            QList<qint64> offsets; fx.opt.onWindow = [&](const QString &f, qint64 off, qint64) { if (f == kMain + ".01") offsets << off; };
            const auto r = fx.run(); QVERIFY2(r.result == Result::Done, qPrintable(r.error));
            QCOMPARE(offsets.first(), qint64(0));
            QCOMPARE(slurp(fx.dst(kMain + ".01")), slurp(fx.src.path() + "/" + kMain + ".01"));
        }
    }
    // A resumed partial is trusted only as far as it equals the source: the header check alone let a right-header,
    // wrong-body partial through, and verifyStore then hashed the bad copy and agreed with itself.
    void resumeKeepsOnlyThePrefixThatEqualsTheSource() {
        struct Case { const char *name; std::function<QByteArray(QByteArray)> damage; qint64 expectedRestart; };
        const QList<Case> cases = {
            {"garbage body", [](QByteArray b) { b = b.left(2048); b.replace(1024, 1024, QByteArray(1024, 'g')); return b; }, 1024},
            {"zeroed middle window", [](QByteArray b) { b = b.left(3072); b.replace(1024, 1024, QByteArray(1024, '\0')); return b; }, 1024},
            {"flip in the last kept window", [](QByteArray b) { b = b.left(2048); b[2000] = char(b[2000] ^ 0x5a); return b; }, 1536},
            {"older build, same header", [](QByteArray b) { b = b.left(2048); for (int i = 0x100; i < b.size(); ++i) b[i] = char(b[i] + 1); return b; }, 0},
        };
        for (const Case &c : cases) {
            Fx fx; QDir().mkpath(fx.root() + "/" + fx.id);
            QVERIFY(QFile::setPermissions(fx.root(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
            const QString name = kMain + ".02", srcBytesPath = fx.src.path() + "/" + name;
            { QFile f(fx.dst(name + ".partial")); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(c.damage(slurp(srcBytesPath))); f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner); }
            QList<qint64> offsets; fx.opt.onWindow = [&](const QString &f, qint64 off, qint64) { if (f == name) offsets << off; };
            const auto r = fx.run(); QVERIFY2(r.result == Result::Done, qPrintable(r.error));
            QVERIFY2(!offsets.isEmpty() && offsets.first() == c.expectedRestart, c.name);
            QCOMPARE(slurp(fx.dst(name)), slurp(srcBytesPath));
            QVERIFY2(verifyStore(fx.root(), fx.id).result == Result::Done, c.name);
        }
    }
    void resumeAgainstAShorterOrChangedSourceRestarts() {
        Fx fx; QDir().mkpath(fx.root() + "/" + fx.id);
        QVERIFY(QFile::setPermissions(fx.root(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const QString name = kMain + ".01";
        { QFile f(fx.dst(name + ".partial")); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(slurp(fx.src.path() + "/" + name) + QByteArray(1024, 'z')); f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner); }
        QList<qint64> offsets; fx.opt.onWindow = [&](const QString &f, qint64 off, qint64) { if (f == name) offsets << off; };
        const auto r = fx.run(); QVERIFY2(r.result == Result::Done, qPrintable(r.error));
        QCOMPARE(offsets.first(), qint64(0)); QCOMPARE(slurp(fx.dst(name)), slurp(fx.src.path() + "/" + name));
    }
    void cancelWhileCheckingThePartialLeavesItAlone() {
        Fx fx; QDir().mkpath(fx.root() + "/" + fx.id);
        QVERIFY(QFile::setPermissions(fx.root(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const QString name = kMain + ".01"; const QByteArray good = slurp(fx.src.path() + "/" + name).left(2048) + QByteArray(1024, 'g'); // a bad tail: finishing the check would cut it
        { QFile f(fx.dst(name + ".partial")); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(good); f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner); }
        QVERIFY(QFile::copy(fx.src.path() + "/" + kMain, fx.dst(kMain))); // already final, so the first work is checking the partial
        std::atomic<bool> cancel(false); fx.opt.pauseMs = 1; fx.opt.sleepFn = [&](int) { cancel = true; }; // fires between windows of the check
        QCOMPARE(fx.run(&cancel).result, Result::Cancelled);
        QCOMPARE(slurp(fx.dst(name + ".partial")), good); // the check cancelled before truncating anything
    }
    void rejectsUnusableWindowAndBlockSizes() {
        for (const auto &sizes : QList<QPair<qint64, qint64>>{{0, 512}, {1024, 0}, {-1, 512}, {1024, kMaxBlockBytes + 1}, {1024, qint64(1) << 31}}) {
            Fx fx; fx.opt.windowBytes = sizes.first; fx.opt.blockBytes = sizes.second;
            const auto r = fx.run(); QCOMPARE(r.result, Result::Failed); QVERIFY(!QFileInfo::exists(fx.root()));
        }
    }
    // The check of a resumed partial reports progress and pauses per window of bytes, whatever the block size is.
    void checkingAPartialReportsProgressAndPausesPerWindowOfBytes() {
        Fx fx; fx.opt.blockBytes = 300; fx.opt.pauseMs = 5;     // 300 does not divide the 1024-byte window
        QDir().mkpath(fx.root() + "/" + fx.id);
        QVERIFY(QFile::setPermissions(fx.root(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const QString name = kMain + ".01";
        QVERIFY(QFile::copy(fx.src.path() + "/" + kMain, fx.dst(kMain)));          // final already, so its check is the first work
        { QFile f(fx.dst(name + ".partial")); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(slurp(fx.src.path() + "/" + name).left(3072)); f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner); }
        bool copying = false; int checkPauses = 0; QList<qint64> checkProgress;
        fx.opt.sleepFn = [&](int) { if (!copying) ++checkPauses; };
        fx.opt.onWindow = [&](const QString &, qint64, qint64) { copying = true; };
        const auto r = fx.run(nullptr, [&](const Progress &p) { if (!copying && p.fileName == name) checkProgress << p.fileBytes; });
        QVERIFY2(r.result == Result::Done, qPrintable(r.error));
        QCOMPARE(checkPauses, 5);                       // main: after 1200, 2400, 3600 bytes; partial: after 1200 and 2400
        QVERIFY(checkProgress.size() >= 4); QVERIFY(checkProgress.contains(300)); QVERIFY(checkProgress.contains(2400));
    }
    // A final file left behind before the manifest was written is compared with the source too: a planted file with a
    // valid header and a wrong body used to be accepted, hashed into the manifest and then "verified" against itself.
    void aFinalFileWithoutAManifestMustEqualTheSource() {
        Fx fx; QDir().mkpath(fx.root() + "/" + fx.id);
        QVERIFY(QFile::setPermissions(fx.root(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const QString name = kMain + ".01"; QByteArray planted = slurp(fx.src.path() + "/" + name);
        planted.replace(1024, 1024, QByteArray(1024, 'g'));
        { QFile f(fx.dst(name)); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(planted); f.setPermissions(QFileDevice::ReadOwner); }
        const auto r = fx.run(); QCOMPARE(r.result, Result::Failed); QVERIFY2(r.error.contains("differs from the source"), qPrintable(r.error));
        QCOMPARE(slurp(fx.dst(name)), planted);              // never replaced or deleted
        QVERIFY(!QFileInfo::exists(fx.dst("store.json")));
        // a file the copy itself completed before being cut short is still accepted
        Fx ok; std::atomic<bool> cancel(false);
        QCOMPARE(ok.run(&cancel, [&](const Progress &p) { if (p.fileName == kMain + ".02") cancel = true; }).result, Result::Cancelled);
        QVERIFY(QFileInfo::exists(ok.dst(kMain + ".01")) && !QFileInfo::exists(ok.dst("store.json")));
        { std::atomic<bool> c2(false); ok.opt.pauseMs = 1; ok.opt.sleepFn = [&](int) { c2 = true; };      // cancelled while a done file is being compared
          QCOMPARE(ok.run(&c2).result, Result::Cancelled); ok.opt.pauseMs = 0; ok.opt.sleepFn = nullptr; }
        QList<qint64> reported; const auto done = ok.run(nullptr, [&](const Progress &p) { if (p.fileName == kMain + ".01") reported << p.fileBytes; });
        QVERIFY2(done.result == Result::Done, qPrintable(done.error)); QVERIFY(!reported.isEmpty() && reported.last() == 4096);
        QVERIFY(verifyStore(ok.root(), ok.id).result == Result::Done);
    }
    void alreadyCompleteStoreIsNotCopiedAgain() {
        Fx fx; QVERIFY(fx.run().result == Result::Done);
        int windows = 0; fx.opt.onWindow = [&](const QString &, qint64, qint64) { ++windows; };
        QVERIFY(fx.run().result == Result::Done); QCOMPARE(windows, 0);
    }
    void refusesSymlinkedSourceAndMismatchedSubCaches() {
        { Fx fx; QTemporaryDir evil; const QString f = fx.src.path() + "/" + kMain + ".02";
          QVERIFY(QFile::copy(f, evil.path() + "/real")); QVERIFY(QFile::remove(f)); QVERIFY(QFile::link(evil.path() + "/real", f));
          const auto r = fx.run(); QCOMPARE(r.result, Result::Failed); QVERIFY(!QFileInfo::exists(fx.dst(kMain))); }
        { Fx fx; QTemporaryDir other; makeFakeCache(other.path(), "arm64e", 9, kSubs, 4096);
          QVERIFY(QFile::remove(fx.src.path() + "/" + kMain + ".03")); QVERIFY(QFile::copy(other.path() + "/" + kMain + ".03", fx.src.path() + "/" + kMain + ".03"));
          const auto r = fx.run(); QCOMPARE(r.result, Result::Failed); QVERIFY(r.error.contains("does not belong")); QVERIFY(!QFileInfo::exists(fx.root() + "/" + fx.id)); }
        { Fx fx; const auto r = copyCache(fx.src.path(), "arm64", fx.root(), fx.opt); QCOMPARE(r.result, Result::Failed); }
        { Fx fx; QVERIFY(QFile::remove(fx.src.path() + "/" + kMain + ".01")); QCOMPARE(fx.run().result, Result::Failed); }
    }
    void refusesWhenSpaceIsShortOrAnotherCopyRuns() {
        { Fx fx; fx.opt.freeBytes = [](const QString &) { return qint64(4 * 4096 - 1); };
          const auto r = fx.run(); QCOMPARE(r.result, Result::Failed); QVERIFY(r.error.contains("free space")); QVERIFY(!QFileInfo::exists(fx.dst(kMain + ".partial"))); }
        { Fx fx; fx.opt.freeBytes = [](const QString &) { return qint64(4 * 4096); }; QVERIFY(fx.run().result == Result::Done); }
        { Fx fx; QString err; QVERIFY(DyldCacheStore::ensureRoot(fx.root(), &err));
          const int fd = SafeFs::lockFile(fx.root(), ".copy.lock", 0700, &err); QVERIFY(fd >= 0);
          const auto r = fx.run(); QCOMPARE(r.result, Result::Failed); QVERIFY(r.error.contains("Another launcher")); QVERIFY(!QFileInfo::exists(fx.root() + "/" + fx.id)); ::close(fd); }
    }
    void neverReplacesAnExistingStoreFile() {
        Fx fx; QDir().mkpath(fx.root() + "/" + fx.id); QVERIFY(QFile::setPermissions(fx.root(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        { QFile f(fx.dst(kMain + ".01")); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("precious"); }
        const auto r = fx.run(); QCOMPARE(r.result, Result::Failed);
        QCOMPARE(slurp(fx.dst(kMain + ".01")), QByteArray("precious"));
        QVERIFY(!QFileInfo::exists(fx.dst("store.json")));
    }
    void refusesADamagedExistingStore() {
        Fx fx; QVERIFY(fx.run().result == Result::Done);
        { QFile f(fx.dst(kMain + ".02")); QVERIFY(f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)); QVERIFY(f.open(QIODevice::Append)); f.write("x"); }
        int windows = 0; fx.opt.onWindow = [&](const QString &, qint64, qint64) { ++windows; };
        const auto r = fx.run(); QCOMPARE(r.result, Result::Failed); QVERIFY(r.error.contains("damaged")); QCOMPARE(windows, 0);
    }
    void detectsASourceThatChangesWhileCopying() {
        { Fx fx; const QString f = fx.src.path() + "/" + kMain + ".01";
          fx.opt.onWindow = [&](const QString &n, qint64 off, qint64) { if (n == kMain + ".01" && off == 2048) { QFile w(f); w.open(QIODevice::ReadWrite); w.resize(2048); } };
          const auto r = fx.run(); QCOMPARE(r.result, Result::Failed); QVERIFY(!QFileInfo::exists(fx.dst(kMain + ".01"))); }
        { Fx fx; const QString f = fx.src.path() + "/" + kMain + ".01";
          fx.opt.onWindow = [&](const QString &n, qint64 off, qint64) { if (n == kMain + ".01" && off == 3072) { QFile w(f); w.open(QIODevice::Append); w.write("x"); } };
          const auto r = fx.run(); QCOMPARE(r.result, Result::Failed); QVERIFY(r.error.contains("changed")); QVERIFY(!QFileInfo::exists(fx.dst(kMain + ".01"))); }
        { Fx fx; const QString f = fx.src.path() + "/" + kMain + ".01";
          fx.opt.onWindow = [&](const QString &n, qint64 off, qint64) { if (n == kMain + ".01" && off == 0) { QFile w(f); w.open(QIODevice::ReadWrite); w.seek(0x58); w.write("\x7f\x7e", 2); } };
          const auto r = fx.run(); QCOMPARE(r.result, Result::Failed); QVERIFY(r.error.contains("UUID")); QVERIFY(!QFileInfo::exists(fx.dst(kMain + ".01"))); }
    }
    void detectsAPartialThatGrowsWhileCopying() {
        Fx fx; // a guest process shares the user id and can write the partial
        fx.opt.onWindow = [&](const QString &n, qint64 off, qint64) { if (n == kMain + ".01" && off == 3072) { QFile w(fx.dst(kMain + ".01.partial")); w.open(QIODevice::ReadWrite); w.resize(5000); } };
        const auto r = fx.run(); QCOMPARE(r.result, Result::Failed); QVERIFY(r.error.contains("wrong size")); QVERIFY(!QFileInfo::exists(fx.dst(kMain + ".01")));
    }
    void verifyCatchesAFlippedByteThatTheQuickCheckMisses() {
        Fx fx; QVERIFY(fx.run().result == Result::Done);
        QFile f(fx.dst(kMain + ".03")); QVERIFY(f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)); QVERIFY(f.open(QIODevice::ReadWrite));
        f.seek(3000); const QByteArray b = f.read(1); f.seek(3000); f.write(QByteArray(1, char(b[0] ^ 0xff))); f.close();
        QCOMPARE(DyldCacheStore::status(fx.root(), fx.id).state, StoreState::Ready);
        const auto v = verifyStore(fx.root(), fx.id); QCOMPARE(v.result, Result::Failed); QVERIFY(v.error.contains("checksum"));
        std::atomic<bool> cancel(true); QCOMPARE(verifyStore(fx.root(), fx.id, {}, &cancel).result, Result::Cancelled);
    }
};
QTEST_APPLESS_MAIN(DyldCacheCopyTest)
#include "dyldcachecopy_test.moc"
