// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"
#include "appbrowser.h"
#include "prefixdialog.h"
#include "mountdialog.h"
#include <QApplication>
#include <QFile>
#include <QBuffer>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QImage>
#include <QJsonDocument>
#include <QLineEdit>
#include <QLabel>
#include <QTabWidget>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QTextEdit>
#include <QScopeGuard>
#include <QtTest>
#include <QtEndian>

class GuiTest : public QObject {
    Q_OBJECT
private slots:
    void unsuitableMountsExplainDisabledChoices() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8());
        QString mount = temporary.path() + "/recovery"; QVERIFY(QDir().mkpath(mount + "/root/Firmware"));
        Window window({}, false, [mount] { return QList<SourceMount>{{mount, "/dev/synthetic", "fuse", true}}; });
        auto *choices = window.findChild<QComboBox *>("mountedSourceChoices"); QVERIFY(choices); QCOMPARE(choices->count(), 2);
        QVERIFY(choices->currentText().startsWith("No usable sources"));
        QVERIFY(!(choices->model()->flags(choices->model()->index(1, 0)) & Qt::ItemIsEnabled));
        QVERIFY(window.findChild<QLabel *>("mountedSourceSummary")->text().contains("0 suitable"));
    }
    void runtimeDetectionRefresh() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8());
        QString install = temporary.path() + "/runtime";
        QVERIFY(QDir().mkpath(install + "/bin"));
        QFile executable(install + "/bin/darling"); QVERIFY(executable.open(QIODevice::WriteOnly)); executable.close();
        QVERIFY(executable.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        auto previousPath = qgetenv("PATH"); auto restore = qScopeGuard([=] { qputenv("PATH", previousPath); });
        qputenv("PATH", (install + "/bin").toUtf8());
        Window window; window.show();
        auto *runtimes = window.findChild<QComboBox *>("detectedRuntimes"); QVERIFY(runtimes); QCOMPARE(runtimes->count(), 1);
        QVERIFY(QDir().mkpath(install + "/libexec/darling/private/etc"));
        window.findChild<QPushButton *>("Detect paths")->click();
        QCOMPARE(runtimes->count(), 2); QCOMPARE(runtimes->currentIndex(), 1);
        QCOMPARE(window.findChild<QLineEdit *>("darlingField")->text(), executable.fileName());
        QCOMPARE(window.findChild<QLineEdit *>("runtimeRootField")->text(), install);
        QVERIFY(QDir(install + "/libexec/darling/private/etc").removeRecursively());
        window.findChild<QPushButton *>("Detect paths")->click(); QCOMPARE(runtimes->count(), 1); QCOMPARE(runtimes->currentIndex(), 0);
    }
    void iconViewsAndSelection() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QImage image(64, 64, QImage::Format_ARGB32); image.fill(Qt::green);
        QByteArray png; QBuffer buffer(&png); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer, "PNG"));
        QByteArray icns = "icns";
        auto appendLength = [](QByteArray &data, quint32 length) { quint32 big = qToBigEndian(length); data.append(reinterpret_cast<const char *>(&big), 4); };
        appendLength(icns, quint32(16 + png.size())); icns += "icp6"; appendLength(icns, quint32(8 + png.size())); icns += png;
        QString iconPath = temporary.path() + "/App.icns"; QFile file(iconPath); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(icns); file.close();
        AppBrowser browser; browser.resize(500, 350);
        browser.showPreviews({{"Applications/One.app", "Friendly One", iconPath, 1250000}, {"Applications/Two.app", "Two", iconPath, 2000}, {"Applications/Three.app", "Three", {}, 3000}});
        browser.show(); QTest::qWait(30);
        QCOMPARE(browser.item(0)->text(), "Friendly One");
        QCOMPARE(browser.item(0)->icon().pixmap(64, 64).toImage().pixelColor(32, 32), QColor(Qt::green));
        QVERIFY(browser.item(0)->toolTip().contains("MB")); QCOMPARE(browser.iconSize(), QSize(32, 32));
        QTest::mouseClick(browser.viewport(), Qt::LeftButton, Qt::NoModifier, browser.visualItemRect(browser.item(0)).center());
        QTest::keyClick(&browser, Qt::Key_A, Qt::ControlModifier); QCOMPARE(browser.selectedBundles().size(), 3);
        QTest::mouseClick(browser.viewport(), Qt::LeftButton, Qt::NoModifier, browser.visualItemRect(browser.item(0)).center());
        QTest::mouseClick(browser.viewport(), Qt::LeftButton, Qt::ShiftModifier, browser.visualItemRect(browser.item(2)).center()); QCOMPARE(browser.selectedBundles().size(), 3);
        QTest::mouseClick(browser.viewport(), Qt::LeftButton, Qt::ControlModifier, browser.visualItemRect(browser.item(1)).center()); QCOMPARE(browser.selectedBundles().size(), 2);
        browser.setGridView(true); QCOMPARE(browser.viewMode(), QListView::IconMode); QCOMPARE(browser.iconSize(), QSize(64, 64)); QCOMPARE(browser.selectedBundles().size(), 2);
        browser.setGridView(false); QCOMPARE(browser.viewMode(), QListView::ListMode); QCOMPARE(browser.selectedBundles().size(), 2);
    }
    void reuseMountedPartitionAndImport() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8());
        QString first = temporary.path() + "/mounted one", second = temporary.path() + "/mounted two", prefix = temporary.path() + "/prefix";
        QVERIFY(QDir().mkpath(first + "/System/Applications/Utilities/Fixture.app/Contents/MacOS"));
        QVERIFY(QDir().mkpath(first + "/System/Library")); QVERIFY(QDir().mkpath(second + "/usr/lib")); QVERIFY(QDir().mkpath(prefix));
        QFile metadata(first + "/System/Applications/Utilities/Fixture.app/Contents/Info.plist"); QVERIFY(metadata.open(QIODevice::WriteOnly));
        metadata.write("<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>CFBundleExecutable</key><string>Fixture</string><key>CFBundleDisplayName</key><string>Mounted Fixture</string></dict></plist>"); metadata.close();
        QFile executable(first + "/System/Applications/Utilities/Fixture.app/Contents/MacOS/Fixture"); QVERIFY(executable.open(QIODevice::WriteOnly)); executable.write("synthetic private fixture"); executable.close();
        QList<SourceMount> records{{first, "/dev/synthetic1", "fuse", true}, {second, "/dev/synthetic1", "fuse", true}};
        QFile lsblk(temporary.path() + "/lsblk"); QVERIFY(lsblk.open(QIODevice::WriteOnly));
        QByteArray json = QJsonDocument(QJsonObject{{"blockdevices", QJsonArray{QJsonObject{{"path", "/dev/synthetic1"}, {"fstype", "apfs"}, {"mountpoints", QJsonArray{first, second}}}}}}).toJson(QJsonDocument::Compact);
        lsblk.write("#!/bin/sh\nprintf '%s' '" + json + "'\n"); lsblk.close(); QVERIFY(lsblk.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        auto originalPath = qgetenv("PATH"); auto restore = qScopeGuard([=] { qputenv("PATH", originalPath); });
        qputenv("PATH", temporary.path().toUtf8() + ':' + originalPath);
        Window window({}, false, [&records] { return records; }); window.show();
        auto *choices = window.findChild<QComboBox *>("mountedSourceChoices"); QCOMPARE(choices->count(), 3);
        QCOMPARE(window.findChild<QLineEdit *>("volumeField")->text(), QString());
        window.findChild<QPushButton *>("Mount macOS source…")->click();
        auto *dialog = window.findChild<MountDialog *>(); QVERIFY(dialog);
        auto *partitions = dialog->findChild<QComboBox *>("mountPartitions"); QTRY_COMPARE(partitions->count(), 2); partitions->setCurrentIndex(1);
        auto *sources = dialog->findChild<QComboBox *>("existingPartitionSources"); QCOMPARE(sources->count(), 3); QCOMPARE(sources->currentIndex(), 0);
        QVERIFY(!dialog->findChild<QPushButton *>("mountReadOnly")->isEnabled()); QVERIFY(!dialog->findChild<QPushButton *>("useExistingSource")->isEnabled());
        sources->setCurrentIndex(sources->findData(first)); dialog->findChild<QPushButton *>("useExistingSource")->click();
        QCOMPARE(window.findChild<QLineEdit *>("volumeField")->text(), first);
        window.findChild<QLineEdit *>("prefixField")->setText(prefix);
        auto *available = window.findChild<QListWidget *>("availableApps"); QTRY_COMPARE(available->count(), 1); QCOMPARE(available->item(0)->text(), QString("Mounted Fixture"));
        available->item(0)->setSelected(true); window.findChild<QPushButton *>("Import selected apps")->click();
        QTRY_VERIFY(QFileInfo::exists(prefix + "/Applications/Fixture.app/Contents/MacOS/Fixture"));
        QVERIFY(!dialog->isMounting());
        records.removeLast(); QTRY_COMPARE_WITH_TIMEOUT(choices->count(), 2, 5000);
        QSettings("cristim", "darling-launcher").remove("volume");
        Window startup({}, false, [&records] { return records; }); startup.show();
        QCOMPARE(startup.findChild<QLineEdit *>("volumeField")->text(), first);
        QTRY_COMPARE(startup.findChild<QListWidget *>("availableApps")->count(), 1);
        records.clear(); window.findChild<QPushButton *>("Detect mounted macOS volumes")->click(); QCOMPARE(choices->count(), 1);
        auto *refresh = dialog->findChild<QPushButton *>("refreshPartitions"); QVERIFY(refresh); refresh->click(); QTRY_COMPARE(sources->count(), 1); QVERIFY(!dialog->findChild<QPushButton *>("useExistingSource")->isEnabled());
    }
    void settingsAreSeparate() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8());
        Window window; window.show();
        auto *tabs = window.findChild<QTabWidget *>("mainTabs"); QVERIFY(tabs);
        QCOMPARE(tabs->count(), 2); QCOMPARE(tabs->tabText(0), QString("Apps")); QCOMPARE(tabs->tabText(1), QString("Settings"));
        QVERIFY(tabs->widget(1)->isAncestorOf(window.findChild<QLineEdit *>("volumeField")));
        QVERIFY(tabs->widget(1)->isAncestorOf(window.findChild<QPushButton *>("Create prefix")));
        QVERIFY(tabs->widget(0)->isAncestorOf(window.findChild<QListWidget *>("availableApps")));
        QVERIFY(!window.findChild<QWidget *>("contributionPanel")->isVisible());
        QVERIFY(!window.findChild<QPushButton *>("saveProposedIssue")->isVisible());
        tabs->setCurrentIndex(1);
        QVERIFY(window.findChild<QLineEdit *>("volumeField")->isVisible());
        QVERIFY(!window.findChild<QListWidget *>("availableApps")->isVisible());
        window.findChild<QPushButton *>("Apply settings")->click(); QCOMPARE(tabs->currentIndex(), 0);
    }
    void importDiagnoseRetry_data() {
        QTest::addColumn<bool>("retrySuccess");
        QTest::addColumn<bool>("missingLibrary");
        QTest::newRow("workaround-succeeds") << true << false;
        QTest::newRow("workaround-still-fails") << false << false;
        QTest::newRow("missing-library-workaround") << true << true;
    }
    void importDiagnoseRetry() {
        QFETCH(bool, retrySuccess);
        QFETCH(bool, missingLibrary);
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8());
        QString volume = temporary.path() + "/volume", prefix = temporary.path() + "/prefix";
        QString app = volume + "/System/Applications/Test.app";
        QVERIFY(QDir().mkpath(app + "/Contents/MacOS"));
        QVERIFY(QDir().mkpath(volume + "/usr/lib"));
        QVERIFY(QDir().mkpath(prefix));
        QFile plist(app + "/Contents/Info.plist"); QVERIFY(plist.open(QIODevice::WriteOnly));
        plist.write("<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>CFBundleExecutable</key><string>Test</string></dict></plist>"); plist.close();
        QFile executable(app + "/Contents/MacOS/Test"); QVERIFY(executable.open(QIODevice::WriteOnly)); executable.write("fixture"); executable.close();
        QFile library(volume + "/usr/lib/libExample.dylib"); QVERIFY(library.open(QIODevice::WriteOnly)); library.write("fixture"); library.close();
        QString fakePath = temporary.path() + "/fake-darling";
        QFile fake(fakePath); QVERIFY(fake.open(QIODevice::WriteOnly));
        fake.write("#!/bin/sh\nif [ -f \"$DPREFIX/usr/lib/libExample.dylib\" ]; then echo launched; exit 0; fi\n"
                   "printf 'Symbol not found: _Example\\n  Referenced from: /Applications/Test.app/Contents/MacOS/Test\\n  Expected in: /usr/lib/libExample.dylib\\n'\nexit 1\n");
        if (!retrySuccess) { fake.resize(0); fake.seek(0); fake.write("#!/bin/sh\nif [ -f \"$DPREFIX/usr/lib/libExample.dylib\" ]; then echo unrelated failure; exit 2; fi\n"
            "printf 'Symbol not found: _Example\\n  Referenced from: /Applications/Test.app/Contents/MacOS/Test\\n  Expected in: /usr/lib/libExample.dylib\\n'\nexit 1\n"); }
        if (missingLibrary) {
            fake.resize(0); fake.seek(0);
            fake.write("#!/bin/sh\nif [ -f \"$DPREFIX/usr/lib/libExample.dylib\" ]; then echo launched; exit 0; fi\nprintf 'Library not loaded: /usr/lib/libExample.dylib\\n  Referenced from: /Applications/Test.app/Contents/MacOS/Test\\n  Reason: image not found\\n'\nexit 1\n");
        }
        fake.close(); QVERIFY(QFile::setPermissions(fakePath, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        Window window; window.show();
        window.findChild<QLineEdit *>("volumeField")->setText(volume);
        window.findChild<QLineEdit *>("prefixField")->setText(prefix);
        window.findChild<QLineEdit *>("darlingField")->setText(fakePath);
        window.findChild<QLineEdit *>("runtimeRootField")->clear();
        QTimer dismiss;
        connect(&dismiss, &QTimer::timeout, [] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) box->accept();
        });
        dismiss.start(20);
        window.findChild<QPushButton *>("Scan volume")->click();
        auto *available = window.findChild<QListWidget *>("availableApps");
        QTRY_COMPARE_WITH_TIMEOUT(available->count(), 1, 5000);
        available->item(0)->setSelected(true);
        auto *apps = window.findChild<QTableWidget *>("importedApps");
        QMimeData mime; mime.setData("application/x-darling-app-bundles", QJsonDocument(QJsonArray{"System/Applications/Test.app"}).toJson());
        QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(apps->viewport(), &enter); QVERIFY(enter.isAccepted());
        QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(apps->viewport(), &drop); QVERIFY(drop.isAccepted());
        QTRY_COMPARE_WITH_TIMEOUT(apps->rowCount(), 1, 5000);
        QCOMPARE(apps->item(0, 1)->text(), "Applications/Test.app");
        apps->selectRow(0);
        window.findChild<QPushButton *>("Launch selected")->click();
        QTRY_VERIFY_WITH_TIMEOUT(apps->item(0, 2)->text().contains(missingLibrary ? "Library not loaded:" : "Missing _Example"), 5000);
        QVERIFY(!window.findChild<QWidget *>("contributionPanel")->isVisible());
        QVERIFY(window.findChild<QPushButton *>("Import needed library and retry")->isVisible());
        QVERIFY(QFile::rename(volume + "/usr/lib/libExample.dylib", volume + "/usr/lib/temporarily-unavailable"));
        window.findChild<QPushButton *>("Import needed library and retry")->click();
        QVERIFY(!window.findChild<QWidget *>("contributionPanel")->isVisible());
        QVERIFY(QFile::rename(volume + "/usr/lib/temporarily-unavailable", volume + "/usr/lib/libExample.dylib"));
        window.findChild<QPushButton *>("Import needed library and retry")->click();
        QTRY_COMPARE_WITH_TIMEOUT(apps->item(0, 2)->text(), retrySuccess ? QString("Exited successfully") : QString("Exited 2"), 5000);
        QVERIFY(QFileInfo::exists(prefix + "/usr/lib/libExample.dylib"));
        auto catalog = LauncherCore::loadCatalog(prefix);
        QCOMPARE(catalog.value("apps").toArray().first().toObject().value("chain").toArray().size(), 1);
        QVERIFY(window.findChild<QWidget *>("contributionPanel")->isVisible());
        QString explanation = window.findChild<QLabel *>("contributionMessage")->text();
        QVERIFY(explanation.contains("libExample.dylib")); QVERIFY(explanation.contains("nothing is submitted"));
        QVERIFY(!window.findChild<QPushButton *>("Import needed library and retry")->isVisible());
        QVERIFY(!QFileInfo::exists(prefix + "/.darling-launcher/proposed-vibedarling-issue.md"));
        window.findChild<QPushButton *>("saveProposedIssue")->click();
        QFile draft(prefix + "/.darling-launcher/proposed-vibedarling-issue.md"); QVERIFY(draft.open(QIODevice::ReadOnly));
        QVERIFY(draft.readAll().contains(missingLibrary ? "Library not loaded: /usr/lib/libExample.dylib" : "Symbol not found: _Example"));
        auto savedApps = catalog.value("apps").toArray(); auto savedApp = savedApps.first().toObject();
        savedApp.insert("chain", QJsonArray{}); savedApps[0] = savedApp; catalog.insert("apps", savedApps);
        QString error; QVERIFY(LauncherCore::saveCatalog(prefix, catalog, &error));
        window.findChild<QPushButton *>("Scan volume")->click(); apps->selectRow(0);
        window.findChild<QPushButton *>("Launch selected")->click();
        QTRY_COMPARE_WITH_TIMEOUT(apps->item(0, 2)->text(), retrySuccess ? QString("Exited successfully") : QString("Exited 2"), 5000);
        QVERIFY(!window.findChild<QWidget *>("contributionPanel")->isVisible());
    }
    void oneAuthorizationGuiAndCloseGuard() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8());
        QFile lsblk(temporary.path() + "/lsblk"); QVERIFY(lsblk.open(QIODevice::WriteOnly));
        lsblk.write("#!/bin/sh\nprintf '%s' '{\"blockdevices\":[{\"path\":\"/dev/synthetic\",\"fstype\":\"apfs\",\"mountpoints\":[]}]}'\n"); lsblk.close();
        QVERIFY(lsblk.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        QFile pkexec(temporary.path() + "/pkexec"); QVERIFY(pkexec.open(QIODevice::WriteOnly));
        pkexec.write("#!/bin/sh\nprintf x >> \"$LAUNCHER_TEST_AUTH_COUNTER\"\n/usr/bin/sleep 1\nprintf '%s\\n' '{\"event\":\"progress\",\"message\":\"One batch authorized\"}'\nexit 0\n"); pkexec.close();
        QVERIFY(pkexec.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        QByteArray originalPath = qgetenv("PATH"), oldCounter = qgetenv("LAUNCHER_TEST_AUTH_COUNTER");
        auto restore = qScopeGuard([=] { qputenv("PATH", originalPath); qputenv("LAUNCHER_TEST_AUTH_COUNTER", oldCounter); });
        qputenv("PATH", temporary.path().toUtf8() + ':' + originalPath); qputenv("LAUNCHER_TEST_AUTH_COUNTER", (temporary.path() + "/count").toUtf8());
        Window window({}, true); window.show();
        QTRY_VERIFY(window.findChild<MountDialog *>() && window.findChild<MountDialog *>()->isMounting());
        auto *dialog = window.findChild<MountDialog *>();
        dialog->reject(); QVERIFY(dialog->isVisible()); window.close(); QVERIFY(window.isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!dialog->isMounting(), 5000);
        QFile count(temporary.path() + "/count"); QVERIFY(count.open(QIODevice::ReadOnly)); QCOMPARE(count.readAll(), QByteArray("x"));
        QVERIFY(dialog->findChild<QTextEdit *>("mountOutput")->toPlainText().contains("One batch authorized"));
        dialog->reject(); QVERIFY(!dialog->isVisible()); window.close(); QVERIFY(!window.isVisible());
    }
    void explicitMountBatchStartup() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8());
        QFile lsblk(temporary.path() + "/lsblk"); QVERIFY(lsblk.open(QIODevice::WriteOnly));
        lsblk.write("#!/bin/sh\nprintf '%s' '{\"blockdevices\":[]}'\n"); lsblk.close();
        QVERIFY(lsblk.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        QByteArray originalPath = qgetenv("PATH"); auto restore = qScopeGuard([originalPath] { qputenv("PATH", originalPath); });
        qputenv("PATH", temporary.path().toUtf8() + ':' + originalPath);
        Window window({}, true); window.show();
        QTRY_VERIFY(window.findChild<MountDialog *>() != nullptr);
        auto *dialog = window.findChild<MountDialog *>();
        QTRY_VERIFY(dialog->findChild<QTextEdit *>("mountOutput")->toPlainText().contains("No macOS partitions detected"));
        QCOMPARE(window.findChild<QTabWidget *>("mainTabs")->currentIndex(), 1);
    }
    void cloneThenBuildAndDeploy() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8());
        QString fakeGit = temporary.path() + "/git";
        QFile git(fakeGit); QVERIFY(git.open(QIODevice::WriteOnly));
        git.write("#!/bin/sh\nif [ \"$1\" = clone ]; then mkdir -p \"$5\"; echo cloned fixture; exit 0; fi\nexit 1\n"); git.close();
        QVERIFY(git.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        QByteArray originalPath = qgetenv("PATH"); auto restore = qScopeGuard([originalPath] { qputenv("PATH", originalPath); }); qputenv("PATH", temporary.path().toUtf8() + ':' + originalPath);
        Window window(QString(TEST_SOURCE_DIR) + "/tests/fixtures/prefix_builder.py"); window.show();
        window.findChild<QLineEdit *>("volumeField")->clear();
        window.findChild<QPushButton *>("Create prefix")->click();
        auto *dialog = window.findChild<PrefixDialog *>(); QVERIFY(dialog);
        QString source = temporary.path() + "/sources/new clone", workspace = temporary.path() + "/workspace";
        dialog->findChild<QLineEdit *>("prefixBuilderSource")->setText(source);
        dialog->findChild<QLineEdit *>("prefixBuilderWorkspace")->setText(workspace);
        dialog->findChild<QPushButton *>("buildPrefix")->click();
        QTRY_COMPARE_WITH_TIMEOUT(window.findChild<QLineEdit *>("prefixField")->text(), workspace + "/prefix", 5000);
        QVERIFY(QFileInfo(source).isDir()); QCOMPARE(window.findChild<QLineEdit *>("runtimeRootField")->text(), workspace + "/image/usr/local");
        qputenv("PATH", originalPath);
    }
    void prefixBuilderRuntimeSelection() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8());
        QString source = temporary.path() + "/source", workspace = temporary.path() + "/workspace"; QVERIFY(QDir().mkpath(source));
        Window window(QString(TEST_SOURCE_DIR) + "/tests/fixtures/prefix_builder.py"); window.show();
        window.findChild<QLineEdit *>("volumeField")->clear();
        window.findChild<QPushButton *>("Create prefix")->click();
        auto *dialog = window.findChild<PrefixDialog *>(); QVERIFY(dialog);
        dialog->findChild<QLineEdit *>("prefixBuilderSource")->setText(source);
        dialog->findChild<QLineEdit *>("prefixBuilderWorkspace")->setText(workspace);
        dialog->findChild<QComboBox *>("prefixBuilderScope")->setCurrentIndex(1);
        dialog->findChild<QPushButton *>("buildPrefix")->click();
        QTRY_COMPARE_WITH_TIMEOUT(window.findChild<QLineEdit *>("prefixField")->text(), workspace + "/prefix", 5000);
        QCOMPARE(window.findChild<QLineEdit *>("runtimeRootField")->text(), workspace + "/image/usr/local");
        QCOMPARE(window.findChild<QLineEdit *>("darlingField")->text(), workspace + "/build/src/startup/darling");
        window.findChild<QPushButton *>("Initialize selected prefix")->click();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(workspace + "/prefix/runtime-used.txt"), 5000);
        QFile used(workspace + "/prefix/runtime-used.txt"); QVERIFY(used.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromLocal8Bit(used.readAll()).trimmed(), workspace + "/image/usr/local");
        QTRY_VERIFY_WITH_TIMEOUT([&window] {
            for (auto *process : window.findChildren<QProcess *>()) if (process->state() != QProcess::NotRunning) return false;
            return true;
        }(), 5000);
    }
    void closeDuringLaunch() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QString prefix = temporary.path() + "/prefix"; QVERIFY(QDir().mkpath(prefix));
        QString fakePath = temporary.path() + "/fake-darling";
        QFile fake(fakePath); QVERIFY(fake.open(QIODevice::WriteOnly)); fake.write("#!/bin/sh\nexec /usr/bin/sleep 5\n"); fake.close();
        QVERIFY(QFile::setPermissions(fakePath, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QString error; QVERIFY(LauncherCore::saveCatalog(prefix, {{"apps", QJsonArray{QJsonObject{{"name", "Test"}, {"bundle", "Applications/Test.app"}, {"executable", "Test"}}}}}, &error));
        auto *window = new Window; window->show();
        window->findChild<QLineEdit *>("volumeField")->setText(temporary.path());
        window->findChild<QLineEdit *>("prefixField")->setText(prefix);
        window->findChild<QLineEdit *>("darlingField")->setText(fakePath);
        window->findChild<QLineEdit *>("runtimeRootField")->clear();
        window->findChild<QPushButton *>("Scan volume")->click();
        window->findChild<QTableWidget *>("importedApps")->selectRow(0);
        window->findChild<QPushButton *>("Launch selected")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!window->findChildren<QProcess *>().isEmpty() && window->findChildren<QProcess *>().first()->state() == QProcess::Running, 5000);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("QProcess: Destroyed while process .* is still running\\."));
        delete window;
    }
};
QTEST_MAIN(GuiTest)
#include "gui_test.moc"
