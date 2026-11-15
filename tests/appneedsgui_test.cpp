// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen Qt only. Synthetic volume and cache, a fake mount step: no pkexec, no mount, no real Apple data.
#include "appneedsdialog.h"
#include "dyldcache_fixture.h"
#include "macho_fixture.h"
#include "window.h"
#include "dyldcachestore.h"
#include "appbrowser.h"
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QtTest>

using AppNeeds::Item;
using AppNeeds::Status;

static bool put(const QString &path, const QByteArray &data) {
    return QDir().mkpath(QFileInfo(path).absolutePath()) && [&] { QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(data) == data.size(); }();
}
static AppNeeds::Plan samplePlan() {
    AppNeeds::Plan p; p.appName = "Fake"; p.availableBytes = 5LL << 30;
    p.items = {Item{"FakeUI", "/x", {}, Status::Selected, "part of the app's own family", 4, 3, 1, 8192, 100},
               Item{"Shared", "/y", {}, Status::Optional, "linked; not selected unless you choose it", 10, 3, 1, 40960, 900},
               Item{"AppKit", "/z", {}, Status::Skipped, "provided by Darling"}, Item{"Gone", "/w", {}, Status::Skipped, "not on the macOS volume"}};
    return p;
}
struct FakeStep : AutoMount::Step {
    QList<AutoMount::StepRequest> asked; std::function<void()> effect; int exitCode = 0;
    AutoMount::StepResult run(const AutoMount::StepRequest &r) override { asked << r; if (effect) effect(); return {exitCode, false, {}}; }
};

class AppNeedsGuiTest : public QObject {
    Q_OBJECT
    static QPushButton *button(QWidget *w, const char *name) { auto *b = w->findChild<QPushButton *>(name); return b; }
private slots:
    void aCraftedAppNameIsNotRichTextAndImportIsNotTheDefaultButton() {
        AppNeeds::Plan p = samplePlan(); p.appName = "<img src=x>Evil";
        AppNeedsDialog dialog(p);
        QVERIFY(dialog.findChildren<QLabel *>().first()->text().contains("&lt;img src=x&gt;Evil"));
        QVERIFY(!button(&dialog, "needsImport")->isDefault()); QVERIFY(button(&dialog, "needsSkip")->isDefault());
    }
    void theFrameworkDialogDefaultsAndTotals() {
        AppNeedsDialog dialog(samplePlan());
        auto *table = dialog.findChild<QTableWidget *>("needsTable"); QVERIFY(table);
        QCOMPARE(table->rowCount(), 2);
        QCOMPARE(table->item(0, 0)->text(), QString("FakeUI")); QCOMPARE(table->item(0, 0)->checkState(), Qt::Checked);
        QCOMPARE(table->item(1, 0)->text(), QString("Shared")); QCOMPARE(table->item(1, 0)->checkState(), Qt::Unchecked);
        auto *skipped = dialog.findChild<QPlainTextEdit *>("needsSkipped");
        QVERIFY(skipped->toPlainText().contains("AppKit: provided by Darling") && skipped->toPlainText().contains("Gone: not on the macOS volume")); QVERIFY(!skipped->isVisibleTo(&dialog));
        QVERIFY(dialog.findChild<QLabel *>("needsTotals")->text().contains("Selected: 1 frameworks, 4 files"));
        QVERIFY(button(&dialog, "needsImport")->isEnabled());
        table->item(1, 0)->setCheckState(Qt::Checked);
        QCOMPARE(dialog.plan().selectedNames(), (QStringList{"FakeUI", "Shared"})); QVERIFY(dialog.findChild<QLabel *>("needsTotals")->text().contains("Selected: 2 frameworks, 14 files"));
        table->item(0, 0)->setCheckState(Qt::Unchecked); table->item(1, 0)->setCheckState(Qt::Unchecked);
        QVERIFY(!button(&dialog, "needsImport")->isEnabled()); QCOMPARE(dialog.plan().selectedCount(), 0);
    }
    void importAndSkipAcceptAndReject() {
        AppNeedsDialog a(samplePlan()); QSignalSpy accepted(&a, &QDialog::accepted); button(&a, "needsImport")->click(); QCOMPARE(accepted.count(), 1);
        AppNeedsDialog b(samplePlan()); QSignalSpy rejected(&b, &QDialog::rejected); button(&b, "needsSkip")->click(); QCOMPARE(rejected.count(), 1);
    }
    void theCacheCopyNeedsTheNoticeTickedAndAnOfferablePreview() {
        AppNeedsCache::Preview p; p.cacheApp = true; p.totalBytes = 5LL << 30; p.freeBytes = 12LL << 30; p.files = {{"dyld_shared_cache_arm64e", 573440}, {"dyld_shared_cache_arm64e.01", 1715355648}};
        {
            CacheOfferDialog dialog(p);
            auto *copy = button(&dialog, "cacheCopy"); auto *accept = dialog.findChild<QCheckBox *>("cacheAccept");
            const QString text = dialog.findChild<QTextEdit *>("cacheText")->toPlainText();
            QVERIFY(text.contains("Apple executable programs") && text.contains("never committed") && text.contains("dyld_shared_cache_arm64e.01"));
            QVERIFY(!copy->isEnabled()); accept->setChecked(true); QVERIFY(copy->isEnabled()); accept->setChecked(false); QVERIFY(!copy->isEnabled());
            QVERIFY(!button(&dialog, "cacheMount"));
        }
        AppNeedsCache::Preview blocked = p; blocked.blocker = "The volume that holds the dyld cache (the macOS cryptex OS image) is not mounted.";
        { CacheOfferDialog d(blocked); QVERIFY(!d.findChild<QCheckBox *>("cacheAccept")->isEnabled()); QVERIFY(!button(&d, "cacheCopy")->isEnabled()); QVERIFY(button(&d, "cacheMount")); }
        AppNeedsCache::Preview present = p; present.matchingStore = "arm64e-0123";
        { CacheOfferDialog d(present); QVERIFY(!d.findChild<QCheckBox *>("cacheAccept")->isEnabled()); QVERIFY(!button(&d, "cacheCopy")->isEnabled()); }
    }
    void theMountExplanationDialogSaysWhyAndLetsYouChooseAPartition() {
        const AutoMount::Need need{true, false};
        MountExplainDialog one(need, {{"/dev/nvme0n1p2", "apfs", "Container", false, {}, 245000000000LL}});
        const QString text = one.findChild<QTextEdit *>("mountExplanation")->toPlainText();
        for (const QString &needle : QStringList{"administrator rights", "raw disk device by root", "/dev/nvme0n1p2", "245.0 GB", "Preboot and System volumes", "read-only", "does not write to the macOS partition",
                                                  "does not change the partition table", "copies nothing yet", "separate preview and your confirmation", "until you reboot or unmount them",
                                                  "authentication dialog (polkit)", "never sees or stores your password"})
            QVERIFY2(text.contains(needle), qPrintable(needle));
        QVERIFY(!one.findChild<QComboBox *>("mountPartition"));
        QSignalSpy yes(&one, &QDialog::accepted); button(&one, "mountContinue")->click(); QCOMPARE(yes.count(), 1);
        MountExplainDialog two(need, {{"/dev/a", "apfs", "One", false, {}, 1000000000LL}, {"/dev/b", "apfs", "Two", false, {}, 2000000000LL}});
        auto *combo = two.findChild<QComboBox *>("mountPartition"); QVERIFY(combo); QCOMPARE(combo->count(), 2); QVERIFY(combo->itemText(1).contains("2.0 GB"));
        combo->setCurrentIndex(1); QCOMPARE(two.chosen().device, QString("/dev/b")); QVERIFY(two.findChild<QTextEdit *>("mountExplanation")->toPlainText().contains("/dev/b"));
        QSignalSpy no(&two, &QDialog::rejected); button(&two, "mountCancel")->click(); QCOMPARE(no.count(), 1);
    }
    void windowMountFlowExplainsFirstAndOnlyThenRunsTheStep() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8()); QSettings("cristim", "darling-launcher").clear();
        const QString volume = temporary.path() + "/volume";
        QList<SourceMount> mounts;
        FakeStep step; step.effect = [&] { QVERIFY(put(volume + "/System/Applications/X.app/Contents/Info.plist", "x")); mounts << SourceMount{volume, "dev", "apfs", true}; };
        Window window({}, false, [&mounts] { return mounts; });
        window.autoMountStep = &step; window.partitionProvider = [](QString *) { return QList<MacPartition>{{"/dev/nvme0n1p2", "apfs", "", false, {}, 245000000000LL}}; };
        window.show();
        auto *open = window.findChild<QPushButton *>("autoMountSource"); QVERIFY(open);
        open->click();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<MountExplainDialog *>() && window.findChild<MountExplainDialog *>()->isVisible(), 5000);
        QVERIFY(step.asked.isEmpty());                                        // nothing runs before the explanation is accepted
        window.findChild<MountExplainDialog *>()->findChild<QPushButton *>("mountCancel")->click();
        QVERIFY(step.asked.isEmpty()); QTRY_VERIFY(window.statusBar()->currentMessage().contains("Nothing was mounted"));
        open->click();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<MountExplainDialog *>() && window.findChild<MountExplainDialog *>()->isVisible(), 5000);
        auto *second = window.findChild<MountExplainDialog *>(); second->findChild<QPushButton *>("mountContinue")->click();
        QTRY_COMPARE_WITH_TIMEOUT(step.asked.size(), 1, 5000); QCOMPARE(step.asked[0].device, QString("/dev/nvme0n1p2"));
        QTRY_COMPARE_WITH_TIMEOUT(window.findChild<QLineEdit *>("volumeField")->text(), volume, 5000);
        const int calls = step.asked.size(); open->click();                   // already mounted: no dialog, no helper
        QTest::qWait(200); { auto *shown = window.findChild<MountExplainDialog *>(); QVERIFY(!shown || !shown->isVisible()); } QCOMPARE(step.asked.size(), calls);
    }
    void importingAnAppOffersFrameworkResourcesThenTheCacheAndCopiesOnlyAfterAcceptance_data() { QTest::addColumn<bool>("accept"); QTest::newRow("decline") << false; QTest::newRow("accept") << true; }
    void importingAnAppOffersFrameworkResourcesThenTheCacheAndCopiesOnlyAfterAcceptance() {
        QFETCH(bool, accept);
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8()); QSettings("cristim", "darling-launcher").clear();
        const QString volume = temporary.path() + "/volume", prefix = temporary.path() + "/prefix", cryptex = temporary.path() + "/cryptex", store = temporary.path() + "/store/root";
        const QString app = "System/Applications/Fake.app", ui = "/System/Library/PrivateFrameworks/FakeUI.framework/Versions/A/FakeUI", shared = "/System/Library/Frameworks/Shared.framework/Versions/A/Shared";
        QVERIFY(QDir().mkpath(prefix));
        QVERIFY(put(volume + "/" + app + "/Contents/Info.plist", "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>CFBundleExecutable</key><string>Fake</string></dict></plist>"));
        QVERIFY(put(volume + "/" + app + "/Contents/MacOS/Fake", thin(2, {ui, shared})));
        for (const QString &fw : {"PrivateFrameworks/FakeUI", "Frameworks/Shared"}) {
            const QString base = volume + "/System/Library/" + fw + ".framework/Versions/A/Resources";
            QVERIFY(put(base + "/Info.plist", "<plist/>")); QVERIFY(put(base + "/data.bin", QByteArray(64, 'd')));
        }
        makeFakeCache(cryptex + "/System/Library/dyld", "arm64e", 5, {".01"}, 4096);
        Window window({}, false, [=] { return QList<SourceMount>{{volume, "dev", "apfs", true}, {cryptex, "dmg", "fuse", true}}; });
        window.cacheStoreRoot = store; window.show();
        window.findChild<QLineEdit *>("volumeField")->setText(volume); window.findChild<QLineEdit *>("prefixField")->setText(prefix);
        emit window.findChild<ImportedBrowser *>("importedApps")->bundlesDropped({app});
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<AppNeedsDialog *>() && window.findChild<AppNeedsDialog *>()->isVisible(), 10000);
        auto *needs = window.findChild<AppNeedsDialog *>();
        auto *table = needs->findChild<QTableWidget *>("needsTable");
        QCOMPARE(table->rowCount(), 2); QCOMPARE(table->item(0, 0)->text(), QString("FakeUI")); QCOMPARE(table->item(0, 0)->checkState(), Qt::Checked); // FakeUI is in the app's family
        QCOMPARE(table->item(1, 0)->text(), QString("Shared")); QCOMPARE(table->item(1, 0)->checkState(), Qt::Unchecked);
        QVERIFY(QFile::exists(prefix + "/Applications/Fake.app/Contents/MacOS/Fake"));
        QVERIFY(!QFileInfo::exists(prefix + "/System/Library"));                       // nothing of the frameworks before the user chooses
        needs->findChild<QPushButton *>("needsImport")->click();
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(prefix + "/System/Library/PrivateFrameworks/FakeUI.framework/Versions/A/Resources/data.bin"), 10000);
        QVERIFY(!QFileInfo::exists(prefix + "/System/Library/Frameworks/Shared.framework"));    // optional and not chosen
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<CacheOfferDialog *>() && window.findChild<CacheOfferDialog *>()->isVisible(), 10000);       // the cache is offered only after the resources
        QVERIFY(!QFileInfo::exists(store));
        auto *offer = window.findChild<CacheOfferDialog *>();
        QVERIFY(!offer->findChild<QPushButton *>("cacheCopy")->isEnabled());
        if (!accept) { offer->findChild<QPushButton *>("cacheSkip")->click(); QTest::qWait(100); QVERIFY(!QFileInfo::exists(store)); return; } // declining copies nothing
        offer->findChild<QCheckBox *>("cacheAccept")->setChecked(true); QVERIFY(offer->findChild<QPushButton *>("cacheCopy")->isEnabled());
        offer->findChild<QPushButton *>("cacheCopy")->click();
        QTRY_VERIFY_WITH_TIMEOUT(QDir(store).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size() == 1, 20000);
        const QString id = QDir(store).entryList(QDir::Dirs | QDir::NoDotAndDotDot).first();
        QTRY_VERIFY_WITH_TIMEOUT(DyldCacheStore::status(store, id).state == StoreState::Ready, 20000);
        QFile log(prefix + "/.darling-launcher/app-imports.md");
        QTRY_VERIFY_WITH_TIMEOUT(log.exists() && [&] { QFile f(log.fileName()); return f.open(QIODevice::ReadOnly) && QString::fromUtf8(f.readAll()).contains("| dyld cache |"); }(), 20000);
        QVERIFY(!QFileInfo::exists(prefix + "/.darling-launcher/dyld-cache.json")); // the app links no validated library: the prefix settings stay
    }
};
QTEST_MAIN(AppNeedsGuiTest)
#include "appneedsgui_test.moc"
