// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"
#include "appbrowser.h"
#include "prefixdialog.h"
#include "mountdialog.h"
#include "troubleshooting.h"
#include <QCheckBox>
#include <QCryptographicHash>
#include <QSplitter>
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
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QProgressBar>
#include <QRegularExpression>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QTextEdit>
#include <QScopeGuard>
#include <QSemaphore>
#include <QThreadPool>
#include <QtConcurrent>
#include <QtTest>
#include <QtEndian>

class GuiTest : public QObject {
    Q_OBJECT
    QTemporaryDir isolatedHome;
private slots:
    void init() { QVERIFY(isolatedHome.isValid()); const QString home = isolatedHome.path() + "/" + QString::number(qHash(QString(QTest::currentTestFunction()) + QTest::currentDataTag())); QVERIFY(QDir().mkpath(home)); qputenv("HOME", home.toUtf8()); }
    void workspacePreferencesAndEmptyStates() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8()); QSettings("cristim", "darling-launcher").clear(); QSettings("cristim", "darling-launcher").setValue("recovery/remember", true);
        QSettings settings("cristim", "darling-launcher"); settings.clear();
        const QString first = temporary.path() + "/one", second = temporary.path() + "/two";
        QVERIFY(QDir().mkpath(first)); QVERIFY(QDir().mkpath(second)); settings.setValue("prefix", first); settings.setValue("knownPrefixes", QStringList{first, second});
        QByteArray geometry, splitter;
        {
            Window window({}, false, [] { return QList<SourceMount>{}; }); window.show();
            QVERIFY(window.findChild<QLabel *>("sourceEmptyState")->isVisible()); QVERIFY(window.findChild<QLabel *>("importedEmptyState")->isVisible());
            window.findChild<QPushButton *>("emptyChooseSource")->click(); QVERIFY(window.findChild<QDialog *>("settingsDialog")->isVisible()); window.findChild<QDialog *>("settingsDialog")->hide();
            auto *choices = window.findChild<QComboBox *>("prefixChoices"); QCOMPARE(choices->count(), 2); choices->setCurrentIndex(choices->findData(second)); QVERIFY(QMetaObject::invokeMethod(choices, "activated", Q_ARG(int, choices->currentIndex())));
            QCOMPARE(window.findChild<QLineEdit *>("prefixField")->text(), second); QCOMPARE(choices->toolTip(), second);
            window.findChild<QPushButton *>("gridView")->click(); window.findChild<QComboBox *>("appSort")->setCurrentIndex(3);
            window.resize(1150, 800); window.findChild<QSplitter *>("browserSplitter")->setSizes({300, 650});
            window.close(); geometry = settings.value("windowGeometry").toByteArray(); splitter = settings.value("splitterState").toByteArray(); QVERIFY(!geometry.isEmpty()); QVERIFY(!splitter.isEmpty());
        }
        Window restored({}, false, [] { return QList<SourceMount>{}; }); restored.show();
        QCOMPARE(restored.findChild<QComboBox *>("prefixChoices")->currentData().toString(), second);
        QCOMPARE(restored.findChild<AppBrowser *>("availableApps")->viewMode(), QListView::IconMode);
        QCOMPARE(restored.findChild<ImportedBrowser *>("importedApps")->iconSize(), QSize(64, 64));
        QCOMPARE(restored.findChild<QComboBox *>("appSort")->currentIndex(), 3); QWidget reference; QVERIFY(reference.restoreGeometry(geometry)); QCOMPARE(restored.size(), reference.size().expandedTo(restored.minimumSize()));
        QCOMPARE(settings.value("splitterState").toByteArray(), splitter); QVERIFY(restored.findChild<QSplitter *>("browserSplitter")->restoreState(splitter));
        QVERIFY(!restored.findChild<QDialog *>("settingsDialog")->isVisible());
    }
    void rememberedImportRetriesAndMountLossHidesAction() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8());
        QSettings("cristim", "darling-launcher").clear();
        const QString source = temporary.path() + "/source", prefix = temporary.path() + "/prefix", runtime = temporary.path() + "/darling";
        QVERIFY(QDir().mkpath(source + "/usr/lib")); QVERIFY(QDir().mkpath(prefix));
        QFile library(source + "/usr/lib/fixture.dylib"); QVERIFY(library.open(QIODevice::WriteOnly)); library.write("synthetic fixture"); library.close();
        QFile executable(runtime); QVERIFY(executable.open(QIODevice::WriteOnly)); executable.write("#!/bin/sh\nif [ -f \"$DPREFIX/usr/lib/fixture.dylib\" ]; then echo launched; exit 0; fi\nprintf 'Library not loaded: /usr/lib/fixture.dylib\\n  Referenced from: /Applications/Fixture.app/Contents/MacOS/Fixture\\n  Reason: image not found\\n'\nexit 1\n"); executable.close(); QVERIFY(executable.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QString error; QVERIFY(LauncherCore::saveCatalog(prefix, QJsonObject{{"apps", QJsonArray{QJsonObject{{"name", "Fixture"}, {"bundle", "Applications/Fixture.app"}, {"executable", "Fixture"}}}}}, &error));
        QSettings settings("cristim", "darling-launcher"); settings.setValue("prefix", prefix); settings.setValue("darling", runtime); settings.setValue("volume", source);
        LauncherTroubleshooting::saveRecoveryChoices({true, false, false, true, {}});
        QList<SourceMount> mounts{{source, "/dev/synthetic", "fuse", true}};
        Window window({}, false, [&mounts] { return mounts; }); window.show(); window.findChild<QLineEdit *>("runtimeRootField")->clear();
        auto *apps = window.findChild<ImportedBrowser *>("importedApps"); apps->setCurrentRow(0); window.findChild<QPushButton *>("Launch selected")->click();
        QTRY_VERIFY_WITH_TIMEOUT(apps->status("Applications/Fixture.app").contains("Exited"), 5000);
        QVERIFY(QFileInfo::exists(prefix + "/usr/lib/fixture.dylib"));
        QCOMPARE(LauncherCore::loadCatalog(prefix).value("apps").toArray().first().toObject().value("chain").toArray().size(), 1);
        QVERIFY(!window.findChild<LaunchChoicesDialog *>());
        auto *popup = window.findChild<TroubleshootingDialog *>(); QVERIFY(popup); QVERIFY(popup->findChild<QCheckBox *>("importDependencyOption")->isVisible());
        mounts.clear(); window.findChild<QPushButton *>("Detect mounted macOS volumes")->click();
        QVERIFY(!popup->findChild<QCheckBox *>("importDependencyOption")->isVisible()); QVERIFY(popup->findChild<QPushButton *>("mountDependencySource")->isVisible());
    }

    void dependencyChainReusesActionsAndStartsSeparateAgents() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8()); QSettings settings("cristim", "darling-launcher"); settings.clear();
        const auto path = qgetenv("PATH"); auto restore = qScopeGuard([=] { qputenv("PATH", path); });
        qputenv("HOME", temporary.path().toUtf8()); qputenv("PATH", temporary.path().toUtf8() + ':' + path);
        const QString source = temporary.path() + "/source", prefix = temporary.path() + "/prefix";
        QVERIFY(QDir().mkpath(source + "/usr/lib")); QVERIFY(QDir().mkpath(prefix));
        auto write = [&](const QString &file, const QByteArray &contents) { QFile output(file); if (!output.open(QIODevice::WriteOnly)) return false; output.write(contents); output.close(); return output.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner); };
        QVERIFY(write(source + "/usr/lib/a.dylib", "synthetic A")); QVERIFY(write(source + "/usr/lib/b.dylib", "synthetic B"));
        QVERIFY(write(temporary.path() + "/darling", "#!/bin/sh\nfor name in a b; do if [ ! -f \"$DPREFIX/usr/lib/$name.dylib\" ]; then printf 'Library not loaded: /usr/lib/%s.dylib\\n  Referenced from: /Applications/Fixture.app/Contents/MacOS/Fixture\\n  Reason: image not found\\n' \"$name\"; exit 1; fi; done\necho launched; exit 0\n"));
        QVERIFY(write(temporary.path() + "/codex", "#!/bin/sh\necho started\n/usr/bin/sleep 2\necho finished\n"));
        QVERIFY(write(temporary.path() + "/gh", "#!/bin/sh\nprintf '%s\\n' \"$*\" >> '" + temporary.path().toUtf8() + "/gh-calls'\nexit 0\n"));
        QString error; QVERIFY(LauncherCore::saveCatalog(prefix, QJsonObject{{"apps", QJsonArray{QJsonObject{{"name", "Fixture"}, {"bundle", "Applications/Fixture.app"}, {"executable", "Fixture"}}}}}, &error)); settings.setValue("prefix", prefix); settings.setValue("volume", source); settings.setValue("darling", temporary.path() + "/darling");
        Window window({}, false, [source] { return QList<SourceMount>{{source, "/dev/synthetic", "fuse", true}}; }); window.show(); window.findChild<QLineEdit *>("runtimeRootField")->clear();
        int failurePrompts = 0;
        QTimer approval; connect(&approval, &QTimer::timeout, [&] {
            if (auto *choices = window.findChild<LaunchChoicesDialog *>()) {
                if (choices->property("failurePreferences").toBool()) {
                    ++failurePrompts; choices->findChild<QCheckBox *>("launchImportOption")->setChecked(true); choices->findChild<QCheckBox *>("launchReportOption")->setChecked(true); choices->findChild<QCheckBox *>("launchAiOption")->setChecked(true); choices->findChild<QComboBox *>("launchAgent")->setCurrentText("codex");
                }
                choices->accept();
            }
        }); approval.start(10);
        auto *apps = window.findChild<ImportedBrowser *>("importedApps"); apps->setCurrentRow(0); window.findChild<QPushButton *>("Launch selected")->click();
        QTRY_VERIFY_WITH_TIMEOUT(apps->status("Applications/Fixture.app").contains("Exited successfully"), 5000); QCOMPARE(failurePrompts, 1);
        const auto chain = LauncherCore::loadCatalog(prefix).value("apps").toArray().first().toObject().value("chain").toArray(); QCOMPARE(chain.size(), 2);
        auto jobs = window.findChildren<BackgroundFix *>(); QCOMPARE(jobs.size(), 2); QVERIFY(jobs[0]->isRunning()); QVERIFY(jobs[1]->isRunning()); QVERIFY(jobs[0]->property("workspace") != jobs[1]->property("workspace"));
        QSet<QString> libraries;
        for (auto *job : jobs) { const QString workspace = job->property("workspace").toString(); QVERIFY(workspace.startsWith(temporary.path() + '/')); QFile report(workspace + "/TROUBLESHOOTING.json"); QVERIFY(report.open(QIODevice::ReadOnly)); libraries.insert(QJsonDocument::fromJson(report.readAll()).object().value("missingLibrary").toString()); }
        QCOMPARE(libraries, (QSet<QString>{"/usr/lib/a.dylib", "/usr/lib/b.dylib"}));
        auto drafts = window.findChildren<QDialog *>("issueApprovalDialog"); QCOMPARE(drafts.size(), 1); const QString body = drafts.first()->findChild<QTextEdit *>("issueBody")->toPlainText(); QVERIFY(body.contains("/usr/lib/a.dylib")); QVERIFY(body.contains("/usr/lib/b.dylib"));
        QFile calls(temporary.path() + "/gh-calls"); QVERIFY(calls.open(QIODevice::ReadOnly)); QVERIFY(!calls.readAll().contains("issue create"));
        for (auto *job : jobs) job->stop(); QTRY_VERIFY(!jobs[0]->isRunning() && !jobs[1]->isRunning());
    }

    void failurePreferencesRepeatUntilRemembered_data() {
        QTest::addColumn<QString>("mode");
        for (const QString &mode : {"generic", "dependency", "invalid-runtime", "failed-to-start"}) QTest::newRow(qPrintable(mode)) << mode;
    }
    void failurePreferencesRepeatUntilRemembered() {
        QFETCH(QString, mode);
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8());
        QSettings settings("cristim", "darling-launcher"); settings.clear();
        const QString prefix = temporary.path() + "/prefix", runtime = temporary.path() + "/darling";
        QVERIFY(QDir().mkpath(prefix)); QString error;
        QVERIFY(LauncherCore::saveCatalog(prefix, QJsonObject{{"apps", QJsonArray{QJsonObject{{"name", "Fixture"}, {"bundle", "Applications/Fixture.app"}, {"executable", "Fixture"}}}}}, &error)); settings.setValue("prefix", prefix);
        QFile executable(runtime); QVERIFY(executable.open(QIODevice::WriteOnly));
        executable.write(mode == "failed-to-start" ? "#!/nonexistent/fixture-interpreter\n" : mode == "dependency" ? "#!/bin/sh\nprintf 'Library not loaded: /usr/lib/fixture.dylib\\n  Referenced from: /Applications/Fixture.app/Contents/MacOS/Fixture\\n  Reason: image not found\\n'\nexit 1\n" : "#!/bin/sh\necho unrelated failure\nexit 1\n"); executable.close(); QVERIFY(executable.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QFile lsblk(temporary.path() + "/lsblk"); QVERIFY(lsblk.open(QIODevice::WriteOnly)); lsblk.write("#!/bin/sh\necho '{\"blockdevices\":[]}'\n"); lsblk.close(); QVERIFY(lsblk.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto path = qgetenv("PATH"); auto restore = qScopeGuard([=] { qputenv("PATH", path); }); qputenv("PATH", temporary.path().toUtf8() + ':' + path);
        Window window({}, false, [] { return QList<SourceMount>{}; }); window.show(); window.findChild<QLineEdit *>("darlingField")->setText(mode == "invalid-runtime" ? temporary.path() + "/absent" : runtime); window.findChild<QLineEdit *>("runtimeRootField")->clear();
        auto *apps = window.findChild<ImportedBrowser *>("importedApps"); apps->setCurrentRow(0);
        int before = 0, failures = 0; bool noMacDisabled = false;
        QTimer approvals; connect(&approvals, &QTimer::timeout, [&] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) box->accept();
            if (auto *dialog = window.findChild<LaunchChoicesDialog *>()) {
                if (dialog->property("failurePreferences").toBool()) {
                    ++failures; auto *copy = dialog->findChild<QCheckBox *>("launchImportOption"); noMacDisabled = copy->isVisible() && !copy->isEnabled() && !dialog->findChild<QPushButton *>("launchMountSource")->isVisible();
                    dialog->findChild<QCheckBox *>("rememberLaunchChoices")->setChecked(failures == 2);
                } else ++before;
                dialog->accept();
            }
        }); approvals.start(10);
        QTest::qWait(100);
        window.findChild<QPushButton *>("Launch selected")->click(); QTRY_COMPARE(failures, 1); QCOMPARE(before, 1); QVERIFY(noMacDisabled);
        window.findChild<QPushButton *>("Launch selected")->click(); QTRY_COMPARE(failures, 2); QCOMPARE(before, 2); QVERIFY(LauncherTroubleshooting::recoveryChoices().remember);
        window.findChild<QPushButton *>("Launch selected")->click(); QTest::qWait(200); QCOMPARE(failures, 2); QCOMPARE(before, 2);
        QVERIFY(window.findChild<TroubleshootingDialog *>());
    }

    void invalidRuntimeShowsFailedState() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8()); QSettings("cristim", "darling-launcher").clear(); QSettings("cristim", "darling-launcher").setValue("recovery/remember", true);
        const QString prefix = temporary.path() + "/prefix"; QVERIFY(QDir().mkpath(prefix)); QString error;
        QVERIFY(LauncherCore::saveCatalog(prefix, QJsonObject{{"apps", QJsonArray{QJsonObject{{"name", "Fixture"}, {"bundle", "Applications/Fixture.app"}, {"executable", "Fixture"}}}}}, &error));
        QSettings("cristim", "darling-launcher").setValue("prefix", prefix);
        Window window({}, false, [] { return QList<SourceMount>{}; }); window.show();
        window.findChild<QLineEdit *>("darlingField")->setText(temporary.path() + "/missing-runtime");
        auto *apps = window.findChild<ImportedBrowser *>("importedApps"); apps->setCurrentRow(0);
        QTimer::singleShot(0, &window, [] { for (auto *widget : QApplication::topLevelWidgets()) if (auto *box = qobject_cast<QMessageBox *>(widget)) box->accept(); });
        window.findChild<QPushButton *>("Launch selected")->click(); QCOMPARE(apps->status("Applications/Fixture.app"), "Failed to start");
        QVERIFY(!window.findChild<QTextEdit *>("failureLog")); QVERIFY(!window.findChild<QLabel *>("failureSummary"));
        window.findChild<QPushButton *>("Troubleshoot failed app…")->click(); auto *popup = window.findChild<TroubleshootingDialog *>(); QVERIFY(popup);
        popup->findChild<QPushButton *>("showDependencyDetails")->click(); QVERIFY(popup->findChild<QTextEdit *>("troubleshootingData")->toPlainText().contains("Select an executable"));
        window.findChild<QComboBox *>("appFilter")->setCurrentIndex(1); QVERIFY(apps->item(0)->isHidden()); window.findChild<QComboBox *>("appFilter")->setCurrentIndex(2); QVERIFY(!apps->item(0)->isHidden());
    }
    void browserActivityAndSort() {
        ImportedBrowser browser; browser.showPreviews({{"Applications/B.app", "Beta", {}, 20}, {"Applications/A.app", "Alpha", {}, 10}});
        browser.setSorting(false, false); QCOMPARE(browser.item(0)->text(), "Alpha"); browser.setSorting(true, true); QCOMPARE(browser.item(0)->text(), "Beta");
        browser.setActivity("Applications/A.app", AppBrowser::State::Importing, 50); QVERIFY(browser.status("Applications/A.app").contains("50%"));
        browser.setActivity("Applications/A.app", AppBrowser::State::Launching); browser.setFilter(AppBrowser::Filter::Running); QVERIFY(!browser.item(1)->isHidden()); QVERIFY(browser.item(0)->isHidden());
        browser.setActivity("Applications/A.app", AppBrowser::State::Failed); QVERIFY(browser.item(1)->isHidden()); browser.setFilter(AppBrowser::Filter::Failed); QVERIFY(!browser.item(1)->isHidden());
    }
    void searchAndFilters() {
        AppBrowser browser;
        browser.showPreviews({{"Applications/A.app", "Calculator", {}, 10}, {"Applications/B.app", "Calendar", {}, 20}, {"Applications/C.app", "Notes", {}, 30}});
        browser.setSearch("CAL"); QVERIFY(!browser.item(0)->isHidden()); QVERIFY(!browser.item(1)->isHidden()); QVERIFY(browser.item(2)->isHidden());
        browser.setImportedBundles({"Applications/B.app"}); browser.setFilter(AppBrowser::Filter::Imported);
        QVERIFY(browser.item(0)->isHidden()); QVERIFY(!browser.item(1)->isHidden());
        browser.selectAll(); QCOMPARE(browser.selectedBundles(), QStringList{"Applications/B.app"});
        browser.setSearch({}); browser.setAppState("Applications/C.app", AppBrowser::Filter::Failed); browser.setFilter(AppBrowser::Filter::Failed);
        QVERIFY(!browser.item(2)->isHidden()); QVERIFY(browser.item(1)->isHidden());
        browser.setAppState("Applications/C.app", AppBrowser::Filter::Running); QVERIFY(browser.item(2)->isHidden());
        browser.setFilter(AppBrowser::Filter::Running); QVERIFY(!browser.item(2)->isHidden());
    }

    void unsuitableMountsExplainDisabledChoices() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8()); QSettings("cristim", "darling-launcher").clear(); QSettings("cristim", "darling-launcher").setValue("recovery/remember", true);
        QString mount = temporary.path() + "/recovery"; QVERIFY(QDir().mkpath(mount + "/root/Firmware"));
        Window window({}, false, [mount] { return QList<SourceMount>{{mount, "/dev/synthetic", "fuse", true}}; });
        auto *choices = window.findChild<QComboBox *>("mountedSourceChoices"); QVERIFY(choices); QCOMPARE(choices->count(), 2);
        QVERIFY(choices->currentText().startsWith("No usable sources"));
        QVERIFY(!(choices->model()->flags(choices->model()->index(1, 0)) & Qt::ItemIsEnabled));
        QVERIFY(window.findChild<QLabel *>("mountedSourceSummary")->text().contains("0 suitable"));
    }
    void runtimeDetectionRefresh() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8()); QSettings("cristim", "darling-launcher").clear(); QSettings("cristim", "darling-launcher").setValue("recovery/remember", true);
        QString install = temporary.path() + "/runtime";
        QVERIFY(QDir().mkpath(install + "/bin"));
        QFile executable(install + "/bin/darling"); QVERIFY(executable.open(QIODevice::WriteOnly)); executable.close();
        QVERIFY(executable.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        auto previousPath = qgetenv("PATH"); auto restore = qScopeGuard([=] { qputenv("PATH", previousPath); });
        qputenv("PATH", (install + "/bin").toUtf8());
        Window window; window.show();
        QVERIFY(window.findChild<QLineEdit *>("darlingField")->text().isEmpty());
        QVERIFY(QDir().mkpath(install + "/libexec/darling/private/etc"));
        window.detectRuntime();
        QCOMPARE(window.findChild<QLineEdit *>("darlingField")->text(), executable.fileName());
        QCOMPARE(window.findChild<QLineEdit *>("runtimeRootField")->text(), install);
        QVERIFY(!window.findChild<QWidget *>("Detect paths"));
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
        qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8()); QSettings("cristim", "darling-launcher").clear(); QSettings("cristim", "darling-launcher").setValue("recovery/remember", true);
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
        qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8()); QSettings("cristim", "darling-launcher").clear(); QSettings("cristim", "darling-launcher").setValue("recovery/remember", true);
        Window window; window.show();
        auto *settings = window.findChild<QDialog *>("settingsDialog"); QVERIFY(settings); QVERIFY(!settings->isVisible());
        QVERIFY(settings->isAncestorOf(window.findChild<QLineEdit *>("volumeField")));
        QVERIFY(settings->isAncestorOf(window.findChild<QPushButton *>("Create prefix")));
        QVERIFY(window.findChild<QListWidget *>("availableApps")->isVisible());
        QVERIFY(!window.findChild<QTextEdit *>("failureLog"));
        QVERIFY(!window.findChild<QWidget *>("contributionPanel"));
        window.findChild<QPushButton *>("openSettings")->click(); QVERIFY(settings->isVisible());
        QVERIFY(window.findChild<QLineEdit *>("volumeField")->isVisible());
        QVERIFY(!window.findChild<QWidget *>("Apply settings"));

    }
    void importDiagnoseRetry_data() {
        QTest::addColumn<bool>("retrySuccess");
        QTest::addColumn<bool>("missingLibrary");
        QTest::addColumn<bool>("wrapperStaysRunning");
        QTest::addColumn<bool>("prefixChanges");
        QTest::newRow("workaround-succeeds") << true << false << false << false;
        QTest::newRow("workaround-still-fails") << false << false << false << false;
        QTest::newRow("missing-library-workaround") << true << true << false << false;
        QTest::newRow("loader-error-before-wrapper-exit") << true << true << true << false;
        QTest::newRow("prefix-changes-during-import") << true << true << false << true;
    }
    void importDiagnoseRetry() {
        QFETCH(bool, retrySuccess);
        QFETCH(bool, missingLibrary);
        QFETCH(bool, wrapperStaysRunning);
        QFETCH(bool, prefixChanges);
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8()); QSettings("cristim", "darling-launcher").clear(); QSettings("cristim", "darling-launcher").setValue("recovery/remember", true);
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
        if (wrapperStaysRunning) {
            fake.resize(0); fake.seek(0);
            fake.write("#!/bin/sh\nif [ \"$1\" = shutdown ]; then touch \"$DPREFIX/stopped\"; exit 0; fi\nif [ -f \"$DPREFIX/usr/lib/libExample.dylib\" ]; then echo launched; exit 0; fi\nprintf 'Library not loaded: /usr/lib/libExample.dylib\\n  Referenced from: /Applications/Test.app/Contents/MacOS/Test\\n  Reason: image not found\\n'\nwhile [ ! -f \"$DPREFIX/stopped\" ]; do sleep 0.05; done\nexit 1\n");
        }
        fake.close(); QVERIFY(QFile::setPermissions(fakePath, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        Window window({}, false, [volume] { return QList<SourceMount>{{volume, "/dev/synthetic", "fuse", true}}; }); window.show();
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
        auto *apps = window.findChild<ImportedBrowser *>("importedApps");
        QMimeData mime; mime.setData("application/x-darling-app-bundles", QJsonDocument(QJsonArray{"System/Applications/Test.app"}).toJson());
        QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(apps->viewport(), &enter); QVERIFY(enter.isAccepted());
        QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(apps->viewport(), &drop); QVERIFY(drop.isAccepted());
        QTRY_COMPARE_WITH_TIMEOUT(apps->count(), 1, 5000);
        QCOMPARE(apps->item(0)->data(Qt::UserRole).toString(), "Applications/Test.app");
        apps->setCurrentRow(0);
        QTest::mouseClick(apps->viewport(), Qt::LeftButton, Qt::NoModifier, apps->visualItemRect(apps->item(0)).center());
        LauncherTroubleshooting::saveRecoveryChoices({false, false, false, false, {}});
        int confirmations = 0;
        QTimer confirmation;
        connect(&confirmation, &QTimer::timeout, [&] {
            if (auto *dialog = window.findChild<LaunchChoicesDialog *>("launchChoicesDialog")) {
                ++confirmations;
                dialog->findChild<QCheckBox *>("rememberLaunchChoices")->setChecked(true);
                dialog->accept();
            }
        }); confirmation.start(20);
        QTest::mouseDClick(apps->viewport(), Qt::LeftButton, Qt::NoModifier, apps->visualItemRect(apps->item(0)).center());
        confirmation.stop(); QCOMPARE(confirmations, 1); QVERIFY(LauncherTroubleshooting::recoveryChoices().remember);
        QTRY_VERIFY_WITH_TIMEOUT(apps->status("Applications/Test.app").contains(missingLibrary ? "Needs libExample.dylib" : "Missing _Example"), 5000);
        QVERIFY(!window.findChild<QWidget *>("contributionPanel"));
        QTRY_VERIFY(window.findChild<TroubleshootingDialog *>("troubleshootingDialog"));
        auto *popup = window.findChild<TroubleshootingDialog *>("troubleshootingDialog"); QVERIFY(popup->isVisible());
        QCOMPARE(window.findChildren<TroubleshootingDialog *>().size(), 1);
        QVERIFY(popup->findChild<QLabel *>("dependencySummary")->text().contains("libExample.dylib"));
        QVERIFY(!popup->findChild<QTextEdit *>("troubleshootingData")->isVisible()); popup->findChild<QPushButton *>("showDependencyDetails")->click();
        QVERIFY(popup->findChild<QTextEdit *>("troubleshootingData")->isVisible()); QVERIFY(popup->findChild<QTextEdit *>("troubleshootingData")->toPlainText().contains("Example"));
        QVERIFY(!popup->findChild<QCheckBox *>("importDependencyOption")->isChecked()); QVERIFY(!popup->findChild<QCheckBox *>("reportIssueOption")->isChecked()); QVERIFY(!popup->findChild<QCheckBox *>("aiFixOption")->isChecked());
        auto requestImport = [&] { if (auto *current = window.findChild<TroubleshootingDialog *>("troubleshootingDialog")) { current->findChild<QCheckBox *>("importDependencyOption")->setChecked(true); current->findChild<QPushButton *>("runDependencyActions")->click(); } };
        if (wrapperStaysRunning) {
            QVERIFY(!popup->findChild<QCheckBox *>("importDependencyOption")->isEnabled()); QVERIFY(!QFileInfo::exists(prefix + "/usr/lib/libExample.dylib"));
            popup->findChild<QPushButton *>("stopDependencyPrefix")->click();
            QTRY_VERIFY(QFileInfo::exists(prefix + "/stopped")); QTRY_VERIFY(popup->findChild<QCheckBox *>("importDependencyOption")->isEnabled());
        }
        QVERIFY(QFile::rename(volume + "/usr/lib/libExample.dylib", volume + "/usr/lib/temporarily-unavailable"));
        requestImport();
        QCOMPARE(window.findChild<QProgressBar *>()->maximum(), 0); QVERIFY(window.findChild<QProgressBar *>()->isVisible());
        QTRY_VERIFY(window.statusBar()->currentMessage().contains("Library import failed:"));
        const QString importError = popup->findChild<QLabel *>("dependencyOutcome")->text();
        window.findChild<QPushButton *>("Detect mounted macOS volumes")->click();
        QCOMPARE(popup->findChild<QLabel *>("dependencyOutcome")->text(), importError);
        window.findChild<QDialog *>("settingsDialog")->hide();
        QVERIFY(!window.findChild<QWidget *>("contributionPanel"));
        QVERIFY(QFile::rename(volume + "/usr/lib/temporarily-unavailable", volume + "/usr/lib/libExample.dylib"));
        if (prefixChanges) {
            const QString other = temporary.path() + "/other-prefix"; QVERIFY(QDir().mkpath(other));
            auto *pool = QThreadPool::globalInstance(); pool->waitForDone(); const int maximum = pool->maxThreadCount(); pool->setMaxThreadCount(1);
            QSemaphore started, release;
            auto blocked = QtConcurrent::run([&] { started.release(); release.acquire(); });
            auto unblock = qScopeGuard([&] { release.release(); blocked.waitForFinished(); pool->setMaxThreadCount(maximum); });
            QVERIFY(started.tryAcquire(1, 2000));
            requestImport();
            window.close(); QVERIFY(window.isVisible());
            window.findChild<QLineEdit *>("prefixField")->setText(other);
            release.release(); blocked.waitForFinished();
            QTRY_VERIFY(window.statusBar()->currentMessage().contains("Select that prefix to retry."));
            QVERIFY(!QFileInfo::exists(other + "/usr/lib/libExample.dylib"));
            QVERIFY(LauncherCore::loadCatalog(other).isEmpty());
            window.findChild<QLineEdit *>("prefixField")->setText(prefix);
            window.findChild<QPushButton *>("Scan volume")->click(); apps->setCurrentRow(0);
            window.findChild<QPushButton *>("Launch selected")->click();
        } else requestImport();
        QTRY_COMPARE_WITH_TIMEOUT(apps->status("Applications/Test.app"), retrySuccess ? QString("Exited successfully") : QString("Exited 2"), 5000);
        QVERIFY(QFileInfo::exists(prefix + "/usr/lib/libExample.dylib"));
        auto catalog = LauncherCore::loadCatalog(prefix);
        QCOMPARE(catalog.value("apps").toArray().first().toObject().value("chain").toArray().size(), 1);
        QCOMPARE(catalog.value("apps").toArray().first().toObject().value("chain").toArray().first().toObject().value("sourceLibrary").toString(), volume + "/usr/lib/libExample.dylib");
        if (retrySuccess) QVERIFY(window.statusBar()->currentMessage().contains("succeeded after importing macOS libraries"));
        window.findChild<QPushButton *>("Troubleshoot failed app…")->click(); popup = window.findChild<TroubleshootingDialog *>("troubleshootingDialog"); QVERIFY(popup);
        QString explanation = popup->findChild<QLabel *>("dependencyOutcome")->text(); QVERIFY(explanation.contains("Copied macOS libraries"));
        if (auto *option = popup->findChild<QCheckBox *>("importDependencyOption")) QVERIFY(!option->isEnabled());
        popup->reviewIssue(); auto *issueBody = popup->findChild<QTextEdit *>("issueBody"); QVERIFY(issueBody);
        QVERIFY(issueBody->toPlainText().contains(missingLibrary ? "Library not loaded: /usr/lib/libExample.dylib" : "Symbol not found: _Example"));
        popup->findChild<QDialog *>("issueApprovalDialog")->reject();
        auto savedApps = catalog.value("apps").toArray(); auto savedApp = savedApps.first().toObject();
        savedApp.insert("chain", QJsonArray{}); savedApps[0] = savedApp; catalog.insert("apps", savedApps);
        QString error; QVERIFY(LauncherCore::saveCatalog(prefix, catalog, &error));
        window.findChild<QPushButton *>("Scan volume")->click(); apps->setCurrentRow(0);
        window.findChild<QPushButton *>("Launch selected")->click();
        QTRY_COMPARE_WITH_TIMEOUT(apps->status("Applications/Test.app"), retrySuccess ? QString("Exited successfully") : QString("Exited 2"), 5000);
        QVERIFY(!window.findChild<QWidget *>("contributionPanel"));
        QVERIFY(!window.findChild<QTextEdit *>("failureLog"));
        auto *trash = window.findChild<TrashTarget *>("appTrash"); QVERIFY(trash);
        QMimeData removed; removed.setData("application/x-darling-imported-bundles", QJsonDocument(QJsonArray{"Applications/Test.app"}).toJson());
        QDragEnterEvent trashEnter(QPoint(10, 10), Qt::CopyAction, &removed, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(trash, &trashEnter); QVERIFY(trashEnter.isAccepted());
        QDropEvent trashDrop(QPointF(10, 10), Qt::CopyAction, &removed, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(trash, &trashDrop); QVERIFY(trashDrop.isAccepted());
        QCOMPARE(apps->count(), 0); QVERIFY(QFileInfo::exists(volume + "/System/Applications/Test.app"));
        QVERIFY(window.findChild<QPushButton *>("undoTrash")->isVisible()); window.findChild<QPushButton *>("undoTrash")->click(); QCOMPARE(apps->count(), 1);
        QVERIFY(QFileInfo::exists(prefix + "/Applications/Test.app"));
    }
    void oneAuthorizationGuiAndCloseGuard() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8()); QSettings("cristim", "darling-launcher").clear(); QSettings("cristim", "darling-launcher").setValue("recovery/remember", true);
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
        qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8()); QSettings("cristim", "darling-launcher").clear(); QSettings("cristim", "darling-launcher").setValue("recovery/remember", true);
        QFile lsblk(temporary.path() + "/lsblk"); QVERIFY(lsblk.open(QIODevice::WriteOnly));
        lsblk.write("#!/bin/sh\nprintf '%s' '{\"blockdevices\":[]}'\n"); lsblk.close();
        QVERIFY(lsblk.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        QByteArray originalPath = qgetenv("PATH"); auto restore = qScopeGuard([originalPath] { qputenv("PATH", originalPath); });
        qputenv("PATH", temporary.path().toUtf8() + ':' + originalPath);
        Window window({}, true); window.show();
        QTRY_VERIFY(window.findChild<MountDialog *>() != nullptr);
        auto *dialog = window.findChild<MountDialog *>();
        QTRY_VERIFY(dialog->findChild<QTextEdit *>("mountOutput")->toPlainText().contains("No macOS partitions detected"));
        QVERIFY(window.findChild<QDialog *>("settingsDialog")->isVisible());
    }
    void firstRunOffersManagedCloneBuildAndAdoptsRuntime() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8()); QSettings settings("cristim", "darling-launcher"); settings.clear();
        qputenv("HOME", (temporary.path() + "/managed home").toUtf8()); const QString data = temporary.path() + "/managed home/.darling-launcher";
        QFile git(temporary.path() + "/git"); QVERIFY(git.open(QIODevice::WriteOnly)); git.write("#!/bin/sh\nif [ \"$1\" = clone ]; then /usr/bin/sleep 0.1; /usr/bin/mkdir -p \"$5\"; exit 0; fi\nexit 1\n"); git.close(); QVERIFY(git.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto path = qgetenv("PATH"); auto restore = qScopeGuard([=] { qputenv("PATH", path); }); qputenv("PATH", temporary.path().toUtf8() + ':' + path);
        Window window(QString(TEST_SOURCE_DIR) + "/tests/fixtures/prefix_builder.py", false, [] { return QList<SourceMount>{}; }); window.show(); window.findChild<QLineEdit *>("darlingField")->clear(); window.findChild<QLineEdit *>("runtimeRootField")->clear(); window.findChild<QLineEdit *>("volumeField")->clear();
        window.offerRuntimeSetup(); auto *offer = window.findChild<QDialog *>("firstRunSetup"); QVERIFY(offer && offer->isVisible()); 
        offer->findChild<QPushButton *>("setupBuildDarling")->click(); auto *builder = window.findChild<PrefixDialog *>(); QVERIFY(builder && builder->isVisible());
        QCOMPARE(builder->findChild<QLabel *>("prefixBuilderSource")->text(), data + "/sources/vibedarling");
        const QString workspace = builder->findChild<QLabel *>("prefixBuilderWorkspace")->text(); QVERIFY(workspace.startsWith(data + "/workspaces/"));
        builder->findChild<QComboBox *>("prefixBuilderScope")->setCurrentIndex(1); builder->findChild<QPushButton *>("buildPrefix")->click(); QVERIFY(builder->isBusy()); QVERIFY(!window.close()); builder->close(); QVERIFY(builder->isVisible());
        QTRY_COMPARE_WITH_TIMEOUT(window.findChild<QLineEdit *>("prefixField")->text(), workspace + "/prefix", 5000);
        QCOMPARE(window.findChild<QLineEdit *>("darlingField")->text(), workspace + "/build/src/startup/darling"); QCOMPARE(window.findChild<QLineEdit *>("runtimeRootField")->text(), workspace + "/image/usr/local"); QVERIFY(QFileInfo(data + "/sources/vibedarling").isDir()); QVERIFY(!builder->isBusy());
        QVERIFY(QDir().mkpath(workspace + "/image/usr/local/libexec/darling/private/etc")); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); window.offerRuntimeSetup(); QVERIFY(!window.findChild<QDialog *>("firstRunSetup"));
    }
    void bundledBuilderNeedsNoExternalScript() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8()); QSettings settings("cristim", "darling-launcher"); settings.clear(); qputenv("HOME", temporary.path().toUtf8());
        PrefixDialog builder({}, {}, nullptr);
        const QString script = QDir(temporary.path() + "/.darling-launcher/tools").entryInfoList({"all-vibedarling-pr-prefix-*.py"}).value(0).absoluteFilePath(); QVERIFY(script.startsWith(temporary.path() + "/.darling-launcher/tools/")); QFile file(script); QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex()), QString("be593c04751aa26e3b650d84442b62e2db8eb0b5888f4bc3cd281d4a850ae880"));
        QProcess process; process.start(QStandardPaths::findExecutable("python3"), {script, "--help"}); QVERIFY(process.waitForFinished(3000)); QCOMPARE(process.exitCode(), 0); QVERIFY(process.readAllStandardOutput().contains("resolve-nested"));
        QCOMPARE(builder.findChild<QLabel *>("prefixBuilderSource")->text(), temporary.path() + "/.darling-launcher/sources/vibedarling");
    }

    void cloneThenBuildAndDeploy() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8()); QSettings("cristim", "darling-launcher").clear(); QSettings("cristim", "darling-launcher").setValue("recovery/remember", true);
        QString fakeGit = temporary.path() + "/git";
        QFile git(fakeGit); QVERIFY(git.open(QIODevice::WriteOnly));
        git.write("#!/bin/sh\nif [ \"$1\" = clone ]; then mkdir -p \"$5\"; echo cloned fixture; exit 0; fi\nexit 1\n"); git.close();
        QVERIFY(git.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        QByteArray originalPath = qgetenv("PATH"); auto restore = qScopeGuard([originalPath] { qputenv("PATH", originalPath); }); qputenv("PATH", temporary.path().toUtf8() + ':' + originalPath);
        Window window(QString(TEST_SOURCE_DIR) + "/tests/fixtures/prefix_builder.py"); window.show();
        window.findChild<QLineEdit *>("volumeField")->clear();
        window.findChild<QPushButton *>("Create prefix")->click();
        auto *dialog = window.findChild<PrefixDialog *>(); QVERIFY(dialog);
        QString source = QDir::homePath() + "/.darling-launcher/sources/vibedarling", workspace = dialog->findChild<QLabel *>("prefixBuilderWorkspace")->text();
        dialog->findChild<QPushButton *>("buildPrefix")->click();
        QTRY_COMPARE_WITH_TIMEOUT(window.findChild<QLineEdit *>("prefixField")->text(), workspace + "/prefix", 5000);
        QVERIFY(QFileInfo(source).isDir()); QCOMPARE(window.findChild<QLineEdit *>("runtimeRootField")->text(), workspace + "/image/usr/local");
        qputenv("PATH", originalPath);
    }
    void prefixBuilderRuntimeSelection() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8()); QSettings("cristim", "darling-launcher").clear(); QSettings("cristim", "darling-launcher").setValue("recovery/remember", true);
        QString source = QDir::homePath() + "/.darling-launcher/sources/vibedarling"; QVERIFY(QDir().mkpath(source));
        Window window(QString(TEST_SOURCE_DIR) + "/tests/fixtures/prefix_builder.py"); window.show();
        window.findChild<QLineEdit *>("volumeField")->clear();
        window.findChild<QPushButton *>("Create prefix")->click();
        auto *dialog = window.findChild<PrefixDialog *>(); QVERIFY(dialog);
        const QString workspace = dialog->findChild<QLabel *>("prefixBuilderWorkspace")->text();
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
        window->findChild<ImportedBrowser *>("importedApps")->setCurrentRow(0);
        window->findChild<QPushButton *>("Launch selected")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!window->findChildren<QProcess *>().isEmpty() && window->findChildren<QProcess *>().first()->state() == QProcess::Running, 5000);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("QProcess: Destroyed while process .* is still running\\."));
        delete window;
    }
};
QTEST_MAIN(GuiTest)
#include "gui_test.moc"
