// SPDX-License-Identifier: GPL-3.0-or-later
#include "libraries.h"
#include <QFile>
#include <QDir>
#include <QTemporaryDir>
#include <QtTest>
#include <sys/stat.h>

static const QString kRecap = "/System/Library/PrivateFrameworks/Recap.framework/Versions/A/Recap";
static const QString kSha = QString(64, 'a');

static LibraryRecord record(const QString &name = kRecap) {
    return {name, Libraries::idFor(name), true, {"fixture", "uuid-1", "tool-1", kSha}};
}
static void writeFile(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(data);
}

class LibrariesTest : public QObject {
    Q_OBJECT
private slots:
    void roundTrip() {
        QTemporaryDir dir; QString err; LibrariesFile in; in.enabled = false; in.libraries = {record()};
        QVERIFY2(Libraries::save(dir.path(), in, &err), qPrintable(err));
        LibrariesFile out; QVERIFY2(Libraries::load(dir.path(), &out, &err), qPrintable(err));
        QCOMPARE(out.enabled, false);
        QCOMPARE(out.libraries.size(), 1);
        QCOMPARE(out.libraries[0].installName, kRecap);
        QCOMPARE(out.libraries[0].source.sha256, kSha);
        QCOMPARE(out.libraries[0].id, Libraries::idFor(kRecap));
    }
    void missingFileIsEmptyDefault() {
        QTemporaryDir dir; LibrariesFile out; QString err;
        QVERIFY(Libraries::load(dir.path(), &out, &err));
        QVERIFY(out.enabled); QVERIFY(out.libraries.isEmpty());
    }
    void idIsStableSixteenHex() {
        const QString id = Libraries::idFor(kRecap);
        QCOMPARE(id.size(), 16);
        QCOMPARE(id, Libraries::idFor(kRecap));
        QVERIFY(id != Libraries::idFor(kRecap + "2"));
    }
    void installNameRules_data() {
        QTest::addColumn<QString>("name"); QTest::addColumn<bool>("ok");
        QTest::newRow("system library") << kRecap << true;
        QTest::newRow("usr/lib") << "/usr/lib/libfoo.dylib" << true;
        QTest::newRow("Library/Frameworks") << "/Library/Frameworks/X.framework/X" << true;
        QTest::newRow("relative") << "System/Library/X" << false;
        QTest::newRow("rpath") << "@rpath/X.framework/X" << false;
        QTest::newRow("loader_path") << "@loader_path/../X" << false;
        QTest::newRow("traversal") << "/System/Library/../../etc/passwd" << false;
        QTest::newRow("dot component") << "/System/Library/./X" << false;
        QTest::newRow("empty component") << "/System//Library/X" << false;
        QTest::newRow("outside roots") << "/etc/passwd" << false;
        QTest::newRow("root only") << "/" << false;
        QTest::newRow("nul") << QString("/System/Library/X") + QChar(0) + "y" << false;
    }
    void installNameRules() {
        QFETCH(QString, name); QFETCH(bool, ok); QString err;
        QCOMPARE(Libraries::validInstallName(name, &err), ok);
        if (!ok) QVERIFY(!err.isEmpty());
    }
    void loadRejectsInvalidFiles_data() {
        QTest::addColumn<QByteArray>("json");
        const QString id = Libraries::idFor(kRecap);
        auto rec = [&](const QString &name, const QString &rid, const QString &kind, const QString &sha) {
            return QString(R"({"installName":"%1","id":"%2","enabled":true,"source":{"kind":"%3","sha256":"%4"}})").arg(name, rid, kind, sha);
        };
        auto file = [](const QString &ver, const QString &records) { return QString(R"({"version":%1,"libraries":[%2]})").arg(ver, records).toUtf8(); };
        QTest::newRow("not json") << QByteArray("nope");
        QTest::newRow("version 2") << file("2", rec(kRecap, id, "fixture", kSha));
        QTest::newRow("no version") << QByteArray(R"({"libraries":[]})");
        QTest::newRow("id mismatch") << file("1", rec(kRecap, "0123456789abcdef", "fixture", kSha));
        QTest::newRow("bad kind") << file("1", rec(kRecap, id, "weird", kSha));
        QTest::newRow("bad sha") << file("1", rec(kRecap, id, "fixture", "xyz"));
        QTest::newRow("outside roots") << file("1", rec("/etc/passwd", Libraries::idFor("/etc/passwd"), "fixture", kSha));
        QTest::newRow("duplicate") << file("1", rec(kRecap, id, "fixture", kSha) + "," + rec(kRecap, id, "fixture", kSha));
        QTest::newRow("master enabled as string") << QByteArray(QString(R"({"version":1,"enabled":"false","libraries":[]})").toUtf8());
        QTest::newRow("master enabled missing") << QByteArray(R"({"version":1,"libraries":[]})");
        QTest::newRow("master enabled number") << QByteArray(R"({"version":1,"enabled":0,"libraries":[]})");
        QTest::newRow("libraries not array") << QByteArray(R"({"version":1,"enabled":true,"libraries":{}})");
        QTest::newRow("version as string") << QByteArray(R"({"version":"1","enabled":true,"libraries":[]})");
        QTest::newRow("record enabled as string") << QByteArray(QString(R"({"version":1,"enabled":true,"libraries":[{"installName":"%1","id":"%2","enabled":"false","source":{"kind":"fixture","cacheUUID":"","toolVersion":"","sha256":"%3"}}]})").arg(kRecap, id, kSha).toUtf8());
        QTest::newRow("record enabled null") << QByteArray(QString(R"({"version":1,"enabled":true,"libraries":[{"installName":"%1","id":"%2","enabled":null,"source":{"kind":"fixture","cacheUUID":"","toolVersion":"","sha256":"%3"}}]})").arg(kRecap, id, kSha).toUtf8());
        QTest::newRow("missing source") << QByteArray(QString(R"({"version":1,"enabled":true,"libraries":[{"installName":"%1","id":"%2","enabled":true}]})").arg(kRecap, id).toUtf8());
        QTest::newRow("missing cacheUUID") << QByteArray(QString(R"({"version":1,"enabled":true,"libraries":[{"installName":"%1","id":"%2","enabled":true,"source":{"kind":"fixture","toolVersion":"","sha256":"%3"}}]})").arg(kRecap, id, kSha).toUtf8());
        QTest::newRow("over-long field") << QByteArray(QString(R"({"version":1,"enabled":true,"libraries":[{"installName":"%1","id":"%2","enabled":true,"source":{"kind":"fixture","cacheUUID":"%4","toolVersion":"","sha256":"%3"}}]})").arg(kRecap, id, kSha, QString(300, 'x')).toUtf8());
        QTest::newRow("over-long component") << file("1", rec("/System/Library/" + QString(300, 'a'), Libraries::idFor("/System/Library/" + QString(300, 'a')), "fixture", kSha));
    }
    void loadRejectsInvalidFiles() {
        QFETCH(QByteArray, json);
        QTemporaryDir dir; writeFile(Libraries::librariesPath(dir.path()), json);
        LibrariesFile out; QString err;
        QVERIFY(!Libraries::load(dir.path(), &out, &err));
        QVERIFY(!err.isEmpty());
        QVERIFY(out.libraries.isEmpty());
    }
    void loadRejectsFifoAndSymlink() {
        QTemporaryDir dir; QDir().mkpath(dir.path() + "/.darling-launcher");
        LibrariesFile out; QString err;
        QCOMPARE(mkfifo(qPrintable(Libraries::librariesPath(dir.path())), 0600), 0);
        QVERIFY(!Libraries::load(dir.path(), &out, &err)); // would hang without the regular-file check
        QVERIFY(QFile::remove(Libraries::librariesPath(dir.path())));
        writeFile(dir.path() + "/real.json", R"({"version":1,"enabled":true,"libraries":[]})");
        QVERIFY(QFile::link(dir.path() + "/real.json", Libraries::librariesPath(dir.path())));
        QVERIFY(!Libraries::load(dir.path(), &out, &err));
    }
    void loadRejectsOversizedFile() {
        QTemporaryDir dir; writeFile(Libraries::librariesPath(dir.path()), QByteArray(2 << 20, ' '));
        LibrariesFile out; QString err;
        QVERIFY(!Libraries::load(dir.path(), &out, &err));
    }
    void saveRejectsDuplicateIdAcrossNames() {
        // a forged id collision: two distinct names cannot both carry the same id, because id must equal idFor(name)
        QTemporaryDir dir; QString err; LibrariesFile f; auto a = record(); auto b = record("/usr/lib/libfoo.dylib"); b.id = a.id; f.libraries = {a, b};
        QVERIFY(!Libraries::save(dir.path(), f, &err));
    }
    void saveRejectsInvalidRecord() {
        QTemporaryDir dir; QString err; LibrariesFile f; auto r = record(); r.id = "0123456789abcdef"; f.libraries = {r};
        QVERIFY(!Libraries::save(dir.path(), f, &err));
        QVERIFY(!QFileInfo::exists(Libraries::librariesPath(dir.path())));
    }
    void pinnedList_data() {
        QTest::addColumn<QString>("name"); QTest::addColumn<bool>("pinned");
        QTest::newRow("AppKit") << "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit" << true;
        QTest::newRow("Foundation") << "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation" << true;
        QTest::newRow("CoreFoundation") << "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation" << true;
        QTest::newRow("libobjc") << "/usr/lib/libobjc.A.dylib" << true;
        QTest::newRow("libSystem") << "/usr/lib/libSystem.B.dylib" << true;
        QTest::newRow("Recap is a gap") << kRecap << false;
        QTest::newRow("AppKitScripting not pinned") << "/System/Library/Frameworks/AppKitScripting.framework/AppKitScripting" << false;
        QTest::newRow("other usr/lib") << "/usr/lib/libz.1.dylib" << false;
    }
    void pinnedList() { QFETCH(QString, name); QFETCH(bool, pinned); QCOMPARE(Libraries::isPinned(name), pinned); }
};
QTEST_APPLESS_MAIN(LibrariesTest)
#include "libraries_test.moc"
