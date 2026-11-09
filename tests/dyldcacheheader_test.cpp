// SPDX-License-Identifier: GPL-3.0-or-later
#include "dyldcache_fixture.h"

class DyldCacheHeaderTest : public QObject {
    Q_OBJECT
    static QByteArray mainWith(const QList<FakeSub> &subs, const QString &arch = "arm64e", qint64 size = 4096) {
        return fakeFile(arch, fakeUuid(1), size, subs, 7);
    }
private slots:
    void parsesArchUuidAndSuffixes() {
        const QList<FakeSub> subs = {{".01", fakeUuid(2), 0}, {".02.dylddata", fakeUuid(3), 0}};
        DyldCache::MainHeader h; QString err;
        QVERIFY2(DyldCache::parseMainHeader(mainWith(subs), 4096, &h, &err), qPrintable(err));
        QCOMPARE(h.arch, QString("arm64e")); QCOMPARE(h.uuid, fakeUuid(1));
        QCOMPARE(h.subCaches.size(), 2); QCOMPARE(h.subCaches[1].suffix, QString(".02.dylddata")); QCOMPARE(h.subCaches[0].uuid, fakeUuid(2));
        QVERIFY(DyldCache::parseMainHeader(mainWith({}, "arm64"), 4096, &h, &err)); QCOMPARE(h.arch, QString("arm64"));
    }
    void rejectsBadMagicArchAndZeroUuid() {
        DyldCache::MainHeader h; QString err;
        QByteArray b = mainWith({}); b[0] = 'x'; QVERIFY(!DyldCache::parseMainHeader(b, 4096, &h, &err));
        QVERIFY(!DyldCache::parseMainHeader(mainWith({}, "x86_64"), 4096, &h, &err)); QVERIFY(err.contains("architecture"));
        b = mainWith({}); b.replace(0x58, 16, QByteArray(16, '\0')); QVERIFY(!DyldCache::parseMainHeader(b, 4096, &h, &err));
        QVERIFY(!DyldCache::parseMainHeader(b.left(0x50), 4096, &h, &err));
    }
    void rejectsUnsafeSuffixes() {
        DyldCache::MainHeader h; QString err;
        for (const QString &bad : {QString("../x"), QString(".a/b"), QString("01"), QString(".."), QString(".a..b"), QString("."), QString(".a b")}) {
            QVERIFY2(!DyldCache::parseMainHeader(mainWith({{bad, fakeUuid(2), 0}}), 4096, &h, &err), qPrintable(bad));
        }
        QByteArray b = mainWith({{".01", fakeUuid(2), 0}});
        b.replace(0x200 + 24, 32, QByteArray(32, 'a')); b[0x200 + 24] = '.';   // 32 bytes, no terminator
        QVERIFY(!DyldCache::parseMainHeader(b, 4096, &h, &err)); QVERIFY(err.contains("terminated"));
        QVERIFY(!DyldCache::parseMainHeader(mainWith({{".01", fakeUuid(2), 0}, {".01", fakeUuid(3), 0}}), 4096, &h, &err)); QVERIFY(err.contains("twice"));
    }
    void rejectsTablesOutsideTheHeader() {
        DyldCache::MainHeader h; QString err;
        QByteArray b = mainWith({{".01", fakeUuid(2), 0}});
        QVERIFY(!DyldCache::parseMainHeader(b, 0x200 + 55, &h, &err));      // table past the file
        put32(b, 0x188, 0x8); QVERIFY(!DyldCache::parseMainHeader(b, 4096, &h, &err)); // overlaps the locating fields
        put32(b, 0x188, 0x201); QVERIFY(!DyldCache::parseMainHeader(b, 4096, &h, &err)); // unaligned
        put32(b, 0x188, 0x1000); QVERIFY(!DyldCache::parseMainHeader(b, 1LL << 40, &h, &err)); QVERIFY(err.contains("outside")); // beyond the read window
        b = mainWith({{".01", fakeUuid(2), 0}}); put32(b, 0x18c, 64); QVERIFY(!DyldCache::parseMainHeader(b, 4096, &h, &err)); QVERIFY(err.contains("too many"));
    }
    void fileHeaderGivesUuid() {
        QString arch; QByteArray uuid, err0; QString err;
        QVERIFY(DyldCache::parseFileHeader(mainWith({}), &arch, &uuid, &err)); QCOMPARE(uuid, fakeUuid(1));
        QCOMPARE(DyldCache::uuidHex(uuid).size(), 32);
    }
};
QTEST_APPLESS_MAIN(DyldCacheHeaderTest)
#include "dyldcacheheader_test.moc"
