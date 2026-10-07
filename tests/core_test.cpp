// SPDX-License-Identifier: GPL-3.0-or-later
#include "core.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class CoreTest : public QObject {
    Q_OBJECT
private slots:
    void appMetadataAndSizes() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QString app = temporary.path() + "/Applications/Fallback.app";
        QVERIFY(QDir().mkpath(app + "/Contents/Resources"));
        QFile plist(app + "/Contents/Info.plist"); QVERIFY(plist.open(QIODevice::WriteOnly));
        QByteArray metadata = "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>CFBundleDisplayName</key><string>Friendly Name</string><key>CFBundleIconFile</key><string>AppIcon</string></dict></plist>";
        plist.write(metadata); plist.close();
        QFile icon(app + "/Contents/Resources/AppIcon.icns"); QVERIFY(icon.open(QIODevice::WriteOnly)); icon.write("synthetic image resource"); icon.close();
        auto preview = LauncherCore::appPreview(temporary.path(), "Applications/Fallback.app");
        QCOMPARE(preview.name, "Friendly Name"); QCOMPARE(preview.relativeBundle, "Applications/Fallback.app");
        QCOMPARE(preview.iconPath, icon.fileName()); QCOMPARE(preview.bytes, quint64(metadata.size() + 24));
        QVERIFY(QFile::remove(icon.fileName()));
        QFile outside(temporary.path() + "/outside.icns"); QVERIFY(outside.open(QIODevice::WriteOnly)); outside.write("private"); outside.close();
        QVERIFY(QFile::link(outside.fileName(), icon.fileName()));
        preview = LauncherCore::appPreview(temporary.path(), "Applications/Fallback.app");
        QVERIFY(preview.iconPath.isEmpty()); QCOMPARE(preview.bytes, quint64(metadata.size()));
        QVERIFY(LauncherCore::appPreview(temporary.path(), "../outside.app").relativeBundle.isEmpty());
    }
    void diagnosis() {
        auto result = LauncherCore::diagnose("dyld: Symbol not found: _Example\n  Referenced from: /Applications/Notes.app/Contents/MacOS/Notes\n  Expected in: /System/Library/Frameworks/Example.framework/Versions/A/Example\n");
        QCOMPARE(result.symbol, "_Example");
        QCOMPARE(result.expectedIn, "/System/Library/Frameworks/Example.framework/Versions/A/Example");
        QVERIFY(!LauncherCore::diagnose("unrelated crash").valid());
    }
    void mountLayout() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QVERIFY(!LauncherCore::looksLikeMacVolume(temporary.path()));
        QVERIFY(QDir().mkpath(temporary.path() + "/System/Library"));
        QVERIFY(QDir().mkpath(temporary.path() + "/System/Applications"));
        QVERIFY(LauncherCore::looksLikeMacVolume(temporary.path()));
    }
    void libraryImportAndConfinement() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QString volume = temporary.path() + "/volume", prefix = temporary.path() + "/prefix";
        QVERIFY(QDir().mkpath(volume + "/usr/lib")); QVERIFY(QDir().mkpath(prefix));
        QFile source(volume + "/usr/lib/libExample.dylib"); QVERIFY(source.open(QIODevice::WriteOnly)); source.write("fixture"); source.close();
        QString error;
        QVERIFY2(LauncherCore::importLibrary(volume, prefix, "/usr/lib/libExample.dylib", &error), qPrintable(error));
        QFile imported(prefix + "/usr/lib/libExample.dylib"); QVERIFY(imported.open(QIODevice::ReadOnly)); QCOMPARE(imported.readAll(), QByteArray("fixture"));
        QVERIFY(!LauncherCore::importLibrary(volume, prefix, "/usr/lib/../../escape", &error));
        QVERIFY(!LauncherCore::importLibrary(volume, prefix, "/usr/lib/libExample.dylib", &error));
        QVERIFY(QDir().mkpath(volume + "/System/Library/Frameworks"));
        QVERIFY(QFile::link(temporary.path() + "/outside", volume + "/System/Library/Frameworks/Bad.framework"));
        QVERIFY(!LauncherCore::importLibrary(volume, prefix,
            "/System/Library/Frameworks/Bad.framework/Versions/A/Bad", &error));
        QVERIFY(QDir().mkpath(volume + "/Library/Frameworks/Good.framework"));
        QFile good(volume + "/Library/Frameworks/Good.framework/Good"); QVERIFY(good.open(QIODevice::WriteOnly)); good.write("fixture"); good.close();
        QVERIFY(QFile::link(temporary.path(), prefix + "/Library"));
        QVERIFY(!LauncherCore::importLibrary(volume, prefix,
            "/Library/Frameworks/Good.framework/Good", &error));
        QVERIFY(QFile::remove(prefix + "/Library"));
        QVERIFY2(LauncherCore::importLibrary(volume, prefix,
            "/Library/Frameworks/Good.framework/Good", &error), qPrintable(error));
        QVERIFY(QFileInfo::exists(prefix + "/Library/Frameworks/Good.framework/Good"));
    }
    void catalogBrewfileAndIssue() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QString prefix = temporary.path() + "/prefix"; QVERIFY(QDir().mkpath(prefix));
        QJsonObject catalog{{"apps", QJsonArray{QJsonObject{{"name", "Test"}}}}}; QString error;
        QVERIFY2(LauncherCore::saveCatalog(prefix, catalog, &error), qPrintable(error));
        QCOMPARE(LauncherCore::loadCatalog(prefix), catalog);
        QString source = temporary.path() + "/Brewfile"; QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("brew \"wget\"\n"); file.close();
        QString guest;
        QVERIFY2(LauncherCore::stageBrewfile(prefix, source, &guest, &error), qPrintable(error));
        QCOMPARE(guest, "/.darling-launcher/Brewfile");
        QFile staged(prefix + guest); QVERIFY(staged.open(QIODevice::ReadOnly)); QCOMPARE(staged.readAll(), QByteArray("brew \"wget\"\n")); staged.close();
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); file.write("mas(\"App\", id: 123)\n"); file.close();
        QVERIFY(!LauncherCore::stageBrewfile(prefix, source, &guest, &error));
        AppEntry entry{"Test", "Applications/Test.app", "Test", "System/Applications/Test.app"};
        QString issue = LauncherCore::issueDraft(entry, QJsonArray{QJsonObject{{"symbol", "_Example"}, {"library", "/usr/lib/libExample.dylib"}, {"action", "imported"}}},
            "Symbol not found: _Example", "/mnt/mac", prefix, "/usr/bin/darling");
        QVERIFY(issue.contains("/mnt/mac/System/Applications/Test.app"));
        QVERIFY(issue.contains("_Example expected in /usr/lib/libExample.dylib"));
        QVERIFY(issue.contains("No binary implementation was inspected"));
    }
    void appDiscoveryAndImport() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QString volume = temporary.path() + "/volume", prefix = temporary.path() + "/prefix";
        QString app = volume + "/System/Applications/Test.app";
        QVERIFY(QDir().mkpath(app + "/Contents/MacOS")); QVERIFY(QDir().mkpath(prefix));
        QFile plist(app + "/Contents/Info.plist"); QVERIFY(plist.open(QIODevice::WriteOnly));
        plist.write("<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>CFBundleExecutable</key><string>Test</string></dict></plist>"); plist.close();
        QFile binary(app + "/Contents/MacOS/Test"); QVERIFY(binary.open(QIODevice::WriteOnly)); binary.write("fixture"); binary.close();
        QCOMPARE(LauncherCore::discoverApps(volume), QStringList{"System/Applications/Test.app"});
        AppEntry entry; QString error;
        QVERIFY2(LauncherCore::importApp(volume, prefix, "System/Applications/Test.app", &entry, &error), qPrintable(error));
        QCOMPARE(entry.executable, "Test");
        QCOMPARE(entry.sourceRelative, "System/Applications/Test.app");
        QVERIFY(QFileInfo::exists(prefix + "/Applications/Test.app/Contents/MacOS/Test"));
    }
};
QTEST_GUILESS_MAIN(CoreTest)
#include "core_test.moc"
