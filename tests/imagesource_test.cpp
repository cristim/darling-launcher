// SPDX-License-Identifier: GPL-3.0-or-later
#include "imagesource.h"
#include "macho_fixture.h"
#include <QtTest>

static const QString kA = "/usr/lib/libA.dylib", kB = "/usr/lib/libB.dylib", kC = "/usr/lib/libC.dylib";

// A dishonest source: delivers bytes but records the sha256 of different content.
class LyingSource : public FixtureSource {
public:
    FetchStatus fetch(const QString &name, FetchedImage *out, QString *error) override {
        const FetchStatus st = FixtureSource::fetch(name, out, error);
        if (st == FetchStatus::Ok) out->source.sha256 = ImageSources::sha256Hex("something else");
        return st;
    }
};
class SpySource : public FixtureSource {
public:
    QStringList calls; QString failOn;
    FetchStatus fetch(const QString &name, FetchedImage *out, QString *error) override {
        calls << name;
        if (name == failOn) { *error = "disk on fire"; return FetchStatus::Error; }
        return FixtureSource::fetch(name, out, error);
    }
};

class ImageSourceTest : public QObject {
    Q_OBJECT
private slots:
    void fixtureRoundTrip() {
        FixtureSource s; s.add(kA, "bytes"); FetchedImage img; QString err;
        QVERIFY2(s.fetch(kA, &img, &err) == FetchStatus::Ok, qPrintable(err));
        QCOMPARE(img.bytes, QByteArray("bytes"));
        QCOMPARE(img.source.kind, QString("fixture"));
        QCOMPARE(img.source.sha256, ImageSources::sha256Hex("bytes"));
        QCOMPARE(img.source.sha256.size(), 64);
        QVERIFY2(ImageSources::verifyFetched(kA, &img, &err), qPrintable(err));
    }
    void fixtureMissingNameFails() {
        FixtureSource s; FetchedImage img; QString err;
        QVERIFY(s.fetch(kA, &img, &err) == FetchStatus::NotFound); QVERIFY(!err.isEmpty());
    }
    void verifyRefusesLyingSha() {
        LyingSource s; s.add(kA, "bytes"); FetchedImage img; QString err;
        QVERIFY(s.fetch(kA, &img, &err) == FetchStatus::Ok);
        QVERIFY(!ImageSources::verifyFetched(kA, &img, &err));
        QVERIFY(err.contains("sha256"));
    }
    void verifyRefusesEmptyBytes() {
        FetchedImage img{QByteArray(), {"fixture", "", "", ImageSources::sha256Hex(QByteArray())}}; QString err;
        QVERIFY(!ImageSources::verifyFetched(kA, &img, &err));
        QVERIFY(err.contains("empty"));
    }
    void verifyRefusesBadInstallName_data() {
        QTest::addColumn<QString>("name");
        QTest::newRow("rpath") << "@rpath/X.framework/X";
        QTest::newRow("traversal") << "/usr/lib/../../etc/passwd";
        QTest::newRow("outside roots") << "/etc/passwd";
    }
    void verifyRefusesBadInstallName() {
        QFETCH(QString, name);
        FetchedImage img{"bytes", {"fixture", "", "", ImageSources::sha256Hex("bytes")}}; QString err;
        QVERIFY(!ImageSources::verifyFetched(name, &img, &err));
    }
    void verifyReturnsComputedSha() {
        FetchedImage img{"bytes", {"fixture", "", "", ImageSources::sha256Hex("bytes")}}; QString err;
        QVERIFY(ImageSources::verifyFetched(kA, &img, &err));
        QCOMPARE(img.source.sha256, ImageSources::sha256Hex(QByteArray("bytes")));
        img.source.sha256 = ImageSources::sha256Hex("bytes").toUpper(); // the source's spelling is not kept
        QVERIFY(ImageSources::verifyFetched(kA, &img, &err));
        QCOMPARE(img.source.sha256, ImageSources::sha256Hex(QByteArray("bytes")));
    }
    void closureNeverFetchesInvalidNames() {
        SpySource s; s.add(kA, thin(2, {}));
        ImageClosure c; QString err;
        QVERIFY2(s.closure(thin(2, {"/etc/x", "/usr/lib/../../x", "@rpath/X", kA}), 2, false, &c, &err), qPrintable(err));
        QCOMPARE(s.calls, QStringList({kA}));
        QCOMPARE(c.invalid, QStringList({"/etc/x", "/usr/lib/../../x"}));
        QCOMPARE(c.installNames, QStringList({kA}));
        QVERIFY(c.unresolved.isEmpty());
    }
    void closureFailsClosedOnSourceError() {
        SpySource s; s.add(kA, thin(2, {kB, kC})); s.failOn = kB;
        ImageClosure c; QString err;
        QVERIFY(!s.closure(thin(2, {kA}), 2, false, &c, &err));
        QVERIFY(err.contains("disk on fire"));
        QVERIFY(!s.calls.contains(kC));
    }
    void closureHonoursPreferredSubtype() {
        FixtureSource s; ImageClosure c; QString err;
        QVERIFY(!s.closure(thin(0, {kA}), 2, false, &c, &err));
        QVERIFY(s.closure(thin(0, {kA}), 0, false, &c, &err));
    }
    void closureThroughFixture() {
        FixtureSource s; s.add(kA, thin(2, {kB})); s.add(kB, thin(2, {}));
        ImageClosure c; QString err;
        QVERIFY2(s.closure(thin(2, {kA}), 2, false, &c, &err), qPrintable(err));
        QCOMPARE(c.installNames, QStringList({kA, kB}));
        QVERIFY(c.unresolved.isEmpty());
    }
    void closureReportsMissingAsUnresolved() {
        FixtureSource s; ImageClosure c; QString err;
        QVERIFY2(s.closure(thin(2, {kA}), 2, false, &c, &err), qPrintable(err));
        QCOMPARE(c.unresolved, QStringList({kA}));
    }
    void closureFailsWhenSourceLies() {
        LyingSource s; s.add(kA, thin(2, {})); ImageClosure c; QString err;
        QVERIFY(!s.closure(thin(2, {kA}), 2, false, &c, &err));
        QVERIFY(err.contains("sha256"));
    }
};
QTEST_APPLESS_MAIN(ImageSourceTest)
#include "imagesource_test.moc"
