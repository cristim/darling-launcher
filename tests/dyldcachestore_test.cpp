// SPDX-License-Identifier: GPL-3.0-or-later
#include "dyldcache_fixture.h"
#include <QTemporaryDir>
#include <sys/stat.h>

using namespace DyldCacheStore;

class DyldCacheStoreTest : public QObject {
    Q_OBJECT
    static int modeOf(const QString &p) { struct stat st; return ::stat(p.toUtf8().constData(), &st) == 0 ? int(st.st_mode & 0777) : -1; }
private slots:
    void ensureRootCreatesPrivateAndRefusesOpenOrRelative() {
        QTemporaryDir d; QString err;
        const QString root = d.path() + "/a/b/dyld-cache";
        QVERIFY2(ensureRoot(root, &err), qPrintable(err)); QCOMPARE(modeOf(root), 0700);
        QVERIFY(ensureRoot(root, &err)); // idempotent
        QVERIFY(::chmod(root.toUtf8().constData(), 0755) == 0);
        QVERIFY(!ensureRoot(root, &err)); QCOMPARE(modeOf(root), 0755); // refused, never chmod-ed
        QVERIFY(!ensureRoot("relative/dir", &err));
    }
    void manifestRoundTripAndRejections() {
        QTemporaryDir d;
        const QString id = makeFakeStore(d.path(), "arm64e", 1, {".01", ".02"});
        StoreManifest m; QString err;
        QByteArray raw; { QFile f(d.path() + "/" + id + "/store.json"); QVERIFY(f.open(QIODevice::ReadOnly)); raw = f.readAll(); }
        QVERIFY2(parseManifest(raw, &m, &err), qPrintable(err)); QCOMPARE(m.files.size(), 3);
        QCOMPARE(renderManifest(m), raw);
        auto mutated = [&](const std::function<void(StoreManifest &)> &fn) { StoreManifest c = m; fn(c); StoreManifest out; return parseManifest(renderManifest(c), &out, &err); };
        QVERIFY(!mutated([](StoreManifest &c) { c.files[1].name = "../evil"; }));
        QVERIFY(!mutated([](StoreManifest &c) { c.files[1].name = "other_file"; }));
        QVERIFY(!mutated([](StoreManifest &c) { c.files[2].name = c.files[1].name; }));
        QVERIFY(!mutated([](StoreManifest &c) { c.files.swapItemsAt(0, 1); }));
        QVERIFY(!mutated([](StoreManifest &c) { c.files[1].sha256 = "xyz"; }));
        QVERIFY(!mutated([](StoreManifest &c) { c.files[1].size = 0; }));
        QVERIFY(!mutated([](StoreManifest &c) { c.uuid = "zz"; }));
        QVERIFY(!mutated([](StoreManifest &c) { c.arch = "x86_64"; }));
        QVERIFY(!parseManifest("{\"version\":2}", &m, &err));
        QVERIFY(!parseManifest("not json", &m, &err));
    }
    void statusMatrix() {
        QTemporaryDir d; QString err;
        QCOMPARE(status(d.path(), "arm64e-" + QString(32, 'a')).state, StoreState::Missing);
        const QString id = makeFakeStore(d.path(), "arm64e", 1, {".01"});
        const QString uuid = DyldCache::uuidHex(fakeUuid(100));
        QCOMPARE(status(d.path(), id).state, StoreState::Ready);
        QCOMPARE(status(d.path(), id, uuid).state, StoreState::Ready);
        const auto stale = status(d.path(), id, QString(32, 'b')); QCOMPARE(stale.state, StoreState::Stale); QVERIFY(!stale.detail.isEmpty());
        // Partial: no manifest yet
        const QString pid = makeFakeStore(d.path(), "arm64", 2, {});
        QVERIFY(QFile::remove(d.path() + "/" + pid + "/store.json")); QCOMPARE(status(d.path(), pid).state, StoreState::Partial);
        // Corrupt: size, header uuid, wrong manifest for the directory, symlinked entry, invalid id
        const QString f = d.path() + "/" + id + "/dyld_shared_cache_arm64e.01";
        { QFile w(f); QVERIFY(w.open(QIODevice::Append)); w.write("x"); }
        QCOMPARE(status(d.path(), id).state, StoreState::Corrupt);
        { QFile w(f); QVERIFY(w.open(QIODevice::ReadWrite)); w.resize(4096); w.seek(0x58); w.write("\x01\x02", 2); }
        QCOMPARE(status(d.path(), id).state, StoreState::Corrupt);
        const QString other = makeFakeStore(d.path(), "arm64e", 3, {});
        QVERIFY(QFile::copy(d.path() + "/" + other + "/store.json", d.path() + "/" + pid + "/store.json"));
        QCOMPARE(status(d.path(), pid).state, StoreState::Corrupt);
        const QString moved = "arm64e-" + QString(32, 'e'); // a complete store copied under another id must not pass
        QVERIFY(QDir().mkpath(d.path() + "/" + moved));
        for (const QString &n : QDir(d.path() + "/" + other).entryList(QDir::Files)) QVERIFY(QFile::copy(d.path() + "/" + other + "/" + n, d.path() + "/" + moved + "/" + n));
        QCOMPARE(status(d.path(), other).state, StoreState::Ready);
        QCOMPARE(status(d.path(), moved).state, StoreState::Corrupt);
        QTemporaryDir evil; QVERIFY(QFile::link(evil.path(), d.path() + "/arm64e-" + QString(32, 'c')));
        QCOMPARE(status(d.path(), "arm64e-" + QString(32, 'c')).state, StoreState::Corrupt);
        QCOMPARE(status(d.path(), "../x").state, StoreState::Corrupt);
    }
    void statusRefusesSymlinkedFilesAndKeepsPathsOutOfDetails() {
        QTemporaryDir d, evil;
        const QString id = makeFakeStore(d.path(), "arm64e", 1, {".01"});
        const QString f = d.path() + "/" + id + "/dyld_shared_cache_arm64e.01";
        QVERIFY(QFile::copy(f, evil.path() + "/real")); QVERIFY(QFile::remove(f)); QVERIFY(QFile::link(evil.path() + "/real", f));
        const auto st = status(d.path(), id);
        QCOMPARE(st.state, StoreState::Corrupt);
        QVERIFY(!st.detail.contains(d.path())); QVERIFY(!st.detail.contains(evil.path()));
    }
    void listStoresSkipsSymlinksAndOddNames() {
        QTemporaryDir d, evil;
        const QString a = makeFakeStore(d.path(), "arm64e", 1, {}), b = makeFakeStore(d.path(), "arm64", 2, {});
        QVERIFY(QFile::link(evil.path(), d.path() + "/arm64e-" + QString(32, 'd')));
        QVERIFY(QDir().mkpath(d.path() + "/not-a-store"));
        QCOMPARE(listStores(d.path()), QStringList({b, a})); // "arm64-" sorts before "arm64e-"
        QCOMPARE(listStores(d.path() + "/missing"), QStringList());
    }
    void freeSpaceNeedsSizePlusMargin() {
        QTemporaryDir d; QString err, seen;
        const FreeBytesFn fn = [&](const QString &dir) { seen = dir; return qint64(1000); };
        QVERIFY(checkFreeSpace(d.path(), 900, 100, &err, fn)); // exactly enough
        QVERIFY(!checkFreeSpace(d.path(), 901, 100, &err, fn)); QVERIFY(err.contains("free space"));
        QVERIFY(!checkFreeSpace(d.path(), 1, 1000, &err, fn));
        QVERIFY(checkFreeSpace(d.path() + "/not/yet/there", 1, 1, &err, fn)); QCOMPARE(seen, d.path()); // nearest existing ancestor
        QVERIFY(!checkFreeSpace(d.path(), 1, 1, &err, [](const QString &) { return qint64(-1); }));
        QVERIFY(freeBytes(d.path()) > 0);
    }
    void guestDirPrefixesCanonicalHostPathAndCapsLength() {
        QTemporaryDir d, link; QString err, guest;
        const QString id = makeFakeStore(d.path(), "arm64e", 1, {});
        QVERIFY2(guestDir(d.path(), id, &guest, &err), qPrintable(err));
        QCOMPARE(guest, QString(kGuestHostRoot) + QFileInfo(d.path() + "/" + id).canonicalFilePath());
        QVERIFY(QFile::link(d.path(), link.path() + "/l"));
        QVERIFY(guestDir(link.path() + "/l", id, &guest, &err)); QVERIFY(!guest.contains("/l/")); // symlinked root is resolved
        QVERIFY(!guestDir(d.path(), "arm64e-" + QString(32, 'f'), &guest, &err));   // missing
        QString deep = d.path(); while (deep.size() < 190) deep += "/dddddddddd";
        const QString deepId = makeFakeStore(deep, "arm64e", 1, {});
        QVERIFY(!guestDir(deep, deepId, &guest, &err)); QVERIFY(err.contains("200"));
    }
};
QTEST_APPLESS_MAIN(DyldCacheStoreTest)
#include "dyldcachestore_test.moc"
