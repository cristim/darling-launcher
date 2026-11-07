// SPDX-License-Identifier: GPL-3.0-or-later
// All images here are synthetic bytes built by this file; no Apple files are read.
#include "macho.h"
#include "macho_fixture.h"
#include <QtTest>

using MachO::kSubtypeArm64;
using MachO::kSubtypeArm64e;

struct FatSlice { quint32 cpu, sub; QByteArray bytes; int offset = -1; int sizeOverride = -1; };
static QByteArray fat(QList<FatSlice> slices) {
    QByteArray b; put32(b, 0xcafebabe, true); put32(b, slices.size(), true);
    int next = 8 + 20 * slices.size(), table = b.size();
    b.resize(next);
    for (auto &s : slices) {
        if (s.offset < 0) s.offset = next;
        next = s.offset + s.bytes.size();
        if (b.size() < next) b.resize(next);
        b.replace(s.offset, s.bytes.size(), s.bytes);
    }
    QByteArray t;
    for (const auto &s : slices) {
        put32(t, s.cpu, true); put32(t, s.sub, true); put32(t, s.offset, true);
        put32(t, s.sizeOverride >= 0 ? s.sizeOverride : s.bytes.size(), true); put32(t, 3, true);
    }
    b.replace(table, t.size(), t);
    return b;
}
static QStringList names(const MachO::Dependencies &d) {
    QStringList n; for (const auto &x : d.dependencies) n << x.installName; return n;
}
static bool read(const QByteArray &img, quint32 pref, bool fallback, MachO::Dependencies *d, QString *err = nullptr) {
    QString local; return MachO::readDependencies(img, pref, fallback, d, err ? err : &local);
}
static const quint32 kArm64 = 0x0100000c;
static const QString kA = "/usr/lib/libA.dylib", kB = "/usr/lib/libB.dylib", kC = "/usr/lib/libC.dylib";

class MachOTest : public QObject {
    Q_OBJECT
private slots:
    void thinArm64e() {
        MachO::Dependencies d;
        QVERIFY(read(thin(2, {kA, kB}), kSubtypeArm64e, false, &d));
        QCOMPARE(names(d), QStringList({kA, kB}));
        QCOMPARE(d.subtype, 2u); QVERIFY(!d.usedFallback);
    }
    void allDylibCommandKindsAndNoOthers() {
        QList<QByteArray> cmds = {dylibCmd(kA, 0xc), dylibCmd(kB, 0x80000018), dylibCmd("/usr/lib/r.dylib", 0x8000001f),
                                  dylibCmd("/usr/lib/l.dylib", 0x20), dylibCmd("/usr/lib/u.dylib", 0x80000023),
                                  dylibCmd("/usr/lib/id.dylib", 0xd)}; // LC_ID_DYLIB is not a dependency
        MachO::Dependencies d;
        QVERIFY(read(thinWith(2, cmds), kSubtypeArm64e, false, &d));
        QCOMPARE(d.dependencies.size(), 5);
        QVERIFY(d.dependencies[1].weak); QVERIFY(!d.dependencies[0].weak);
        QVERIFY(!names(d).contains("/usr/lib/id.dylib"));
    }
    void maskedSubtype() {
        MachO::Dependencies d;
        QVERIFY(read(thin(0x81000002, {kA}), kSubtypeArm64e, false, &d));
        QCOMPARE(d.subtype, 2u);
    }
    void fatPicksPreferredSlice() {
        const QByteArray img = fat({{kArm64, 0, thin(0, {kA})}, {kArm64, 2, thin(2, {kB})}});
        MachO::Dependencies d;
        QVERIFY(read(img, kSubtypeArm64e, false, &d)); QCOMPARE(names(d), QStringList({kB})); QVERIFY(!d.usedFallback);
        QVERIFY(read(img, kSubtypeArm64, false, &d)); QCOMPARE(names(d), QStringList({kA}));
    }
    void fallbackOnlyWhenAsked() {
        const QByteArray img = fat({{kArm64, 0, thin(0, {kA})}});
        MachO::Dependencies d; QString err;
        QVERIFY(!read(img, kSubtypeArm64e, false, &d, &err));
        QVERIFY(read(img, kSubtypeArm64e, true, &d)); QVERIFY(d.usedFallback); QCOMPARE(d.subtype, 0u);
        QVERIFY(!read(thin(0, {kA}), kSubtypeArm64e, false, &d));
        QVERIFY(read(thin(0, {kA}), kSubtypeArm64e, true, &d)); QVERIFY(d.usedFallback);
    }
    void rejectsOtherCpuAndMagic() {
        MachO::Dependencies d;
        QVERIFY(!read(thinWith(2, {dylibCmd(kA)}, 0x01000007), kSubtypeArm64e, true, &d));
        QByteArray bad = thin(2, {kA}); bad[0] = 0;
        QVERIFY(!read(bad, kSubtypeArm64e, true, &d));
        QVERIFY(!read(QByteArray(), kSubtypeArm64e, true, &d));
    }
    void truncated() {
        const QByteArray img = thin(2, {kA, kB});
        MachO::Dependencies d;
        for (int len : {3, 20, 31, 40, int(img.size()) - 1}) QVERIFY2(!read(img.left(len), kSubtypeArm64e, false, &d), qPrintable(QString::number(len)));
    }
    void limitsReject_data() {
        QTest::addColumn<QByteArray>("image");
        QList<QByteArray> many(5000, QByteArray());
        for (auto &c : many) { put32(c, 0x32); put32(c, 8); }
        QTest::newRow("ncmds over cap") << thinWith(2, many);
        QByteArray huge; put32(huge, 0x32); put32(huge, 2 << 20); huge.append(QByteArray((2 << 20) - 8, '\0'));
        QTest::newRow("sizeofcmds over cap") << thinWith(2, {huge});
        QByteArray odd = dylibCmd(kA); patch32(odd, 4, quint32(odd.size()) - 4);
        QTest::newRow("cmdsize not multiple of 8") << thinWith(2, {odd});
        QByteArray tiny; put32(tiny, 0x32); put32(tiny, 0);
        QTest::newRow("cmdsize below header") << thinWith(2, {tiny});
        QByteArray past = dylibCmd(kA); patch32(past, 4, quint32(past.size()) + 64);
        QTest::newRow("cmd past sizeofcmds") << (thinWith(2, {past}) + QByteArray(256, '\0'));
        QByteArray offFile = thin(2, {kA}); patch32(offFile, 20, 4096);
        QTest::newRow("sizeofcmds past file") << offFile;
        QByteArray nameAtEnd = dylibCmd(kA); patch32(nameAtEnd, 8, quint32(nameAtEnd.size()));
        QTest::newRow("name offset past cmd") << thinWith(2, {nameAtEnd});
        QByteArray nameInHdr = dylibCmd(kA); patch32(nameInHdr, 8, 8);
        QTest::newRow("name offset inside header") << thinWith(2, {nameInHdr});
        QByteArray noNul; put32(noNul, 0xc); put32(noNul, 32); put32(noNul, 24); put32(noNul, 0); put32(noNul, 0); put32(noNul, 0);
        noNul.append("/usr/lib");
        QTest::newRow("name without NUL") << thinWith(2, {noNul});
        QTest::newRow("name too long") << thinWith(2, {dylibCmd("/" + QString(2000, 'a'))});
        QByteArray t = thin(2, {kA});
        QTest::newRow("fat count zero") << fat({});
        QList<FatSlice> manySlices; for (int i = 0; i < 17; ++i) manySlices.append({kArm64, quint32(i), thin(i, {kA})});
        QTest::newRow("fat count over cap") << fat(manySlices);
        QTest::newRow("fat slice past file") << fat({{kArm64, 2, t, -1, int(t.size()) + 100}});
        QTest::newRow("fat slices overlap") << fat({{kArm64, 0, t, 100}, {kArm64, 2, t, 100 + int(t.size()) - 10}});
        QTest::newRow("fat duplicate slices") << fat({{kArm64, 2, t}, {kArm64, 2, t}});
        QTest::newRow("fat 64 magic") << [] { QByteArray b = fat({{kArm64, 2, thin(2, {kA})}}); patch32(b, 0, 0xcafebabf, true); return b; }();
        QTest::newRow("fat ambiguous fallback") << fat({{kArm64, 1, thin(1, {kA})}, {kArm64, 3, thin(3, {kA})}});
        QTest::newRow("fat arch/subtype disagree") << fat({{kArm64, 0, t}});
    }
    void limitsReject() {
        QFETCH(QByteArray, image); MachO::Dependencies d; QString err;
        QVERIFY(!MachO::readDependencies(image, kSubtypeArm64e, true, &d, &err));
        QVERIFY(!err.isEmpty());
    }
    void fat64IsNamedInError() {
        QByteArray b = fat({{kArm64, 2, thin(2, {kA})}}); patch32(b, 0, 0xcafebabf, true);
        MachO::Dependencies d; QString err;
        QVERIFY(!read(b, kSubtypeArm64e, true, &d, &err));
        QVERIFY(err.contains("FAT_MAGIC_64"));
    }
};
QTEST_APPLESS_MAIN(MachOTest)
#include "macho_test.moc"
