// SPDX-License-Identifier: GPL-3.0-or-later
#include "core.h"
#include "sources.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class CoreTest : public QObject {
    Q_OBJECT
private slots:
    void trashAndRestore() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        const QString prefix = temporary.path() + "/prefix", bundle = "Applications/Test.app";
        QVERIFY(QDir().mkpath(prefix + '/' + bundle));
        QFile data(prefix + '/' + bundle + "/private.txt"); QVERIFY(data.open(QIODevice::WriteOnly)); data.write("private fixture"); data.close();
        QJsonObject record{{"bundle", bundle}, {"name", "Test"}, {"chain", QJsonArray{QJsonObject{{"library", "/usr/lib/fixture"}}}}};
        QString error; QVERIFY(LauncherCore::saveCatalog(prefix, QJsonObject{{"apps", QJsonArray{record}}}, &error));
        LauncherCore::TrashReceipt receipt;
        QVERIFY(!LauncherCore::trashApp(prefix, "Applications/../Test.app", &error));
        QVERIFY(!LauncherCore::trashApp(prefix, "Applications/Unknown.app", &error));
        QVERIFY(LauncherCore::trashApp(prefix, bundle, &error, &receipt));
        QVERIFY(!QFileInfo::exists(data.fileName())); QVERIFY(QFileInfo::exists(receipt.trashedPath + "/private.txt"));
        QVERIFY(LauncherCore::loadCatalog(prefix).value("apps").toArray().isEmpty());
        QVERIFY(QDir().mkpath(prefix + '/' + bundle)); QVERIFY(!LauncherCore::restoreApp(receipt, &error));
        QVERIFY(QDir(prefix + '/' + bundle).removeRecursively());
        QVERIFY(LauncherCore::restoreApp(receipt, &error)); QVERIFY(QFileInfo::exists(data.fileName()));
        QCOMPARE(LauncherCore::loadCatalog(prefix).value("apps").toArray().first().toObject(), record);
        QVERIFY(!LauncherCore::restoreApp(receipt, &error));
        QVERIFY(QDir().mkpath(temporary.path() + "/outside"));
        QVERIFY(QDir(prefix + "/.darling-launcher/trash").removeRecursively());
        QVERIFY(QFile::link(temporary.path() + "/outside", prefix + "/.darling-launcher/trash"));
        QVERIFY(!LauncherCore::trashApp(prefix, bundle, &error)); QVERIFY(QFileInfo::exists(data.fileName()));
        QVERIFY(QDir(temporary.path() + "/outside").entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty());
    }

    void mountedSourcesAndContent() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QString first = temporary.path() + "/one volume", second = temporary.path() + "/two", recovery = temporary.path() + "/recovery";
        QVERIFY(QDir().mkpath(first + "/root/System/Applications")); QVERIFY(QDir().mkpath(first + "/root/System/Library"));
        QVERIFY(QDir().mkpath(second + "/usr/lib")); QVERIFY(QDir().mkpath(recovery + "/root/Recovery"));
        QByteArray escaped = first.toUtf8(); escaped.replace(" ", "\\040");
        QByteArray metadata = "10 1 0:3 / " + escaped + " ro,nosuid,nodev,noexec - fuse /dev/synthetic1 ro\n";
        metadata += "11 1 0:4 / " + second.toUtf8() + " ro - apfs /dev/synthetic1 ro\n";
        metadata += "12 1 0:5 / " + recovery.toUtf8() + " ro - fuse /dev/synthetic2 ro\n";
        auto mounts = LauncherSources::parseMountInfo(metadata); QCOMPARE(mounts.size(), 3); QCOMPARE(mounts.first().path, first);
        auto candidates = LauncherSources::candidates(mounts, "/dev/synthetic1"); QCOMPARE(candidates.size(), 2);
        auto wrapped = candidates.first(); QCOMPARE(wrapped.root, first + "/root"); QVERIFY(wrapped.apps); QVERIFY(wrapped.libraries); QVERIFY(wrapped.readOnly);
        QVERIFY(candidates.last().libraries); QVERIFY(!candidates.last().apps);
        QCOMPARE(LauncherSources::automaticSource(candidates, ""), QString());
        QCOMPARE(LauncherSources::automaticSource(candidates, second), second);
        QCOMPARE(LauncherSources::automaticSource({wrapped}, ""), wrapped.root);
        auto unsuitable = LauncherSources::candidates(mounts, "/dev/synthetic2"); QCOMPARE(unsuitable.size(), 1); QVERIFY(unsuitable.first().readable); QVERIFY(!unsuitable.first().usable());
        QVERIFY(LauncherSources::candidates(mounts, "/dev/missing").isEmpty());
        QVERIFY(LauncherSources::parseMountInfo("malformed").isEmpty());
        QVERIFY(QDir().mkpath(temporary.path() + "/outside/Library"));
        QVERIFY(QFile::link(temporary.path() + "/outside", recovery + "/System"));
        QVERIFY(!LauncherSources::candidates(mounts, "/dev/synthetic2").first().usable());
        mounts.append({temporary.path() + "/missing", "/dev/synthetic1", "fuse", true});
        QVERIFY(!LauncherSources::candidates(mounts, "/dev/synthetic1").first().readable);
    }
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
        auto library = LauncherCore::diagnose("dyld: Library not loaded: /System/Library/PrivateFrameworks/Calculate.framework/Versions/A/Calculate\n  Referenced from: /Applications/Calculator.app/Contents/MacOS/Calculator\n  Reason: image not found\n");
        QVERIFY(library.valid()); QVERIFY(library.missingLibrary); QVERIFY(library.symbol.isEmpty());
        QCOMPARE(library.expectedIn, "/System/Library/PrivateFrameworks/Calculate.framework/Versions/A/Calculate");
        QVERIFY(!LauncherCore::diagnose("Library not loaded: /usr/lib/test\nReferenced from: /Applications/Test\nReason: incompatible architecture").valid());
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
        QVERIFY(QDir().mkpath(volume + "/System/Library/PrivateFrameworks/Calculate.framework/Versions/A/Resources"));
        QVERIFY(!LauncherCore::importLibrary(volume, prefix, "/System/Library/PrivateFrameworks/Calculate.framework/Versions/A/Calculate", &error));
        QVERIFY(error.contains("no standalone library file"));
        QVERIFY(!QFileInfo::exists(prefix + "/System/Library/PrivateFrameworks/Calculate.framework"));
    }
    void catalogBrewfileAndIssue() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QString prefix = temporary.path() + "/prefix"; QVERIFY(QDir().mkpath(prefix));
        QJsonObject catalog{{"apps", QJsonArray{QJsonObject{{"name", "Test"}}}}}; QString error;
        QVERIFY2(LauncherCore::saveCatalog(prefix, catalog, &error), qPrintable(error));
        QCOMPARE(LauncherCore::loadCatalog(prefix), catalog);
        QString source = temporary.path() + "/Brewfile"; QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("brew \"wget\"\n"); file.close();
        QString guest, digest;
        QVERIFY2(LauncherCore::stageBrewfile(prefix, source, &guest, &digest, &error), qPrintable(error));
        QCOMPARE(guest, "/.darling-launcher/Brewfile");
        QCOMPARE(digest, QString::fromLatin1(QCryptographicHash::hash("brew \"wget\"\n", QCryptographicHash::Sha256).toHex()));
        QFile staged(prefix + guest); QVERIFY(staged.open(QIODevice::ReadOnly)); QCOMPARE(staged.readAll(), QByteArray("brew \"wget\"\n")); staged.close();
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); file.write("mas(\"App\", id: 123)\n"); file.close();
        QVERIFY(!LauncherCore::stageBrewfile(prefix, source, &guest, &digest, &error));
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
        QString app = volume + "/System/Applications/Utilities/Test.app";
        QVERIFY(QDir().mkpath(app + "/Contents/MacOS")); QVERIFY(QDir().mkpath(prefix));
        QFile plist(app + "/Contents/Info.plist"); QVERIFY(plist.open(QIODevice::WriteOnly));
        plist.write("<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>CFBundleExecutable</key><string>Test</string></dict></plist>"); plist.close();
        QFile binary(app + "/Contents/MacOS/Test"); QVERIFY(binary.open(QIODevice::WriteOnly)); binary.write("fixture"); binary.close();
        QVERIFY(QDir().mkpath(app + "/Contents/Helpers/Internal.app"));
        QVERIFY(QDir().mkpath(temporary.path() + "/outside/Escaped.app"));
        QVERIFY(QFile::link(temporary.path() + "/outside", volume + "/System/Applications/Escape"));
        QCOMPARE(LauncherCore::discoverApps(volume), QStringList{"System/Applications/Utilities/Test.app"});
        AppEntry entry; QString error;
        QVERIFY2(LauncherCore::importApp(volume, prefix, "System/Applications/Utilities/Test.app", &entry, &error), qPrintable(error));
        QCOMPARE(entry.executable, "Test");
        QCOMPARE(entry.sourceRelative, "System/Applications/Utilities/Test.app");
        QVERIFY(QFileInfo::exists(prefix + "/Applications/Test.app/Contents/MacOS/Test"));
    }
};
QTEST_GUILESS_MAIN(CoreTest)
#include "core_test.moc"
