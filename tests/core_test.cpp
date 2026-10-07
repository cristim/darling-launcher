#include "core.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class CoreTest : public QObject {
    Q_OBJECT
private slots:
    void diagnosis() {
        auto result = LauncherCore::diagnose("dyld: Symbol not found: _Example\n  Referenced from: /Applications/Notes.app/Contents/MacOS/Notes\n  Expected in: /System/Library/Frameworks/Example.framework/Versions/A/Example\n");
        QCOMPARE(result.symbol, "_Example");
        QCOMPARE(result.expectedIn, "/System/Library/Frameworks/Example.framework/Versions/A/Example");
        QVERIFY(!LauncherCore::diagnose("unrelated crash").valid());
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
