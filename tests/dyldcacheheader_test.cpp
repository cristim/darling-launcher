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
        putLe32(b, 0x188, 0x8); QVERIFY(!DyldCache::parseMainHeader(b, 4096, &h, &err)); // overlaps the locating fields
        putLe32(b, 0x188, 0x201); QVERIFY(!DyldCache::parseMainHeader(b, 4096, &h, &err)); // unaligned
        putLe32(b, 0x188, 0x1000); QVERIFY(!DyldCache::parseMainHeader(b, 1LL << 40, &h, &err)); QVERIFY(err.contains("beyond the first")); // beyond the bytes that were read
        b = mainWith({{".01", fakeUuid(2), 0}}); putLe32(b, 0x18c, 64); QVERIFY(!DyldCache::parseMainHeader(b, 4096, &h, &err)); QVERIFY(err.contains("too many"));
    }
    // The real table is at 0x392d8 in a 573440-byte file: the bytes read from the start of the file must cover it.
    static QByteArray readWindow(const QByteArray &file) { return file.left(int(qMin<qint64>(DyldCache::kHeaderWindow, file.size()))); }
    void parsesATableAtTheRealOffsetBeyondTheFirst64KiB() {
        const QList<FakeSub> subs = {{".01", fakeUuid(2), 0}, {".02", fakeUuid(3), 0}, {".03", fakeUuid(4), 0}, {".04", fakeUuid(5), 0}, {".05", fakeUuid(6), 0}, {".06", fakeUuid(7), 0},
                                     {".07", fakeUuid(8), 0}, {".08", fakeUuid(9), 0}, {".09", fakeUuid(10), 0}, {".10", fakeUuid(11), 0}, {".11", fakeUuid(12), 0}, {".12", fakeUuid(13), 0}};
        const QByteArray file = fakeFile("arm64e", fakeUuid(1), 573440, subs, 7, kRealTableOffset);
        QVERIFY(kRealTableOffset > 64 * 1024);
        DyldCache::MainHeader h; QString err;
        QVERIFY2(DyldCache::parseMainHeader(readWindow(file), file.size(), &h, &err), qPrintable(err));
        QCOMPARE(h.subCaches.size(), 12); QCOMPARE(h.subCaches.last().suffix, QString(".12")); QCOMPARE(h.subCaches.first().uuid, fakeUuid(2));
    }
    void aTableBeyondTheFileOrBeyondTheWindowIsRefusedWithItsOwnMessage() {
        DyldCache::MainHeader h; QString err;
        const QList<FakeSub> one = {{".01", fakeUuid(2), 0}};
        QByteArray file = fakeFile("arm64e", fakeUuid(1), kRealTableOffset + 56, one, 7, kRealTableOffset);
        QVERIFY(DyldCache::parseMainHeader(readWindow(file), file.size(), &h, &err));
        file.chop(1); // the table's last byte is gone
        QVERIFY(!DyldCache::parseMainHeader(readWindow(file), file.size(), &h, &err)); QVERIFY2(err.contains("outside the cache file"), qPrintable(err));
        const int far = int(DyldCache::kHeaderWindow) - 40; // the entry ends 16 bytes past the window
        file = fakeFile("arm64e", fakeUuid(1), qint64(DyldCache::kHeaderWindow) + 4096, one, 7, far);
        QVERIFY(!DyldCache::parseMainHeader(readWindow(file), file.size(), &h, &err)); QVERIFY2(err.contains("beyond the first 1024 KiB"), qPrintable(err));
    }
    void fileHeaderGivesUuid() {
        QString arch; QByteArray uuid, err0; QString err;
        QVERIFY(DyldCache::parseFileHeader(mainWith({}), &arch, &uuid, &err)); QCOMPARE(uuid, fakeUuid(1));
        QCOMPARE(DyldCache::uuidHex(uuid).size(), 32);
    }
};
QTEST_APPLESS_MAIN(DyldCacheHeaderTest)
#include "dyldcacheheader_test.moc"
