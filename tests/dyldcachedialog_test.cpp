// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen tests of the dyld cache settings page. Everything lives in temporary directories; the fake cache is
// synthetic filler with a valid header. launcher-gui-tests is not used: its closeDuringLaunch test hangs here.
#include "dyldcache_fixture.h"
#include "dyldcachedialog.h"
#include "macho_fixture.h"
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QSemaphore>
#include <QThread>
#include <QThreadPool>

static const QString kLibz = "/usr/lib/libz.1.dylib";
constexpr int kWaitMs = 30000;

struct Env {
    QTemporaryDir volume, stores, prefix;
    QList<QPair<QString, QString>> errors;
    // A volume holding a synthetic cache of three files (main plus two sub-caches), `fileSize` bytes each.
    void makeSource(int seed = 1, qint64 fileSize = 4096) {
        makeFakeCache(volume.path() + "/System/Library/dyld", "arm64e", seed, {".01", ".02"}, fileSize);
    }
    static DyldCacheCopy::Options fast(int pauseMs = 0, qint64 window = 4096) {
        DyldCacheCopy::Options o; o.windowBytes = window; o.pauseMs = pauseMs; o.marginBytes = 0; return o;
    }
    DyldCacheDialog *dialog(const DyldCacheDialog::App &app = {}, std::function<DyldCacheCopy::Options()> options = [] { return fast(); }) {
        auto *d = new DyldCacheDialog(volume.path(), prefix.path(), stores.path(), app);
        d->copyOptions = options;
        d->showError = [this](const QString &t, const QString &x) { errors << qMakePair(t, x); };
        d->show();
        return d;
    }
};
template <class T> static T *child(QWidget *w, const char *name) { auto *c = w->findChild<T *>(name); if (!c) qFatal("no widget %s", name); return c; }
static void idle(DyldCacheDialog *d) { QTRY_VERIFY_WITH_TIMEOUT(!d->isBusy(), kWaitMs); }

class DyldCacheDialogTest : public QObject {
    Q_OBJECT
private slots:
    void constructsAndShowsTheLaunchRuleWithNothingConfigured() {
        Env env; env.makeSource();
        std::unique_ptr<DyldCacheDialog> d(env.dialog()); idle(d.get());
        QVERIFY(child<QLineEdit>(d.get(), "storeLocation")->isReadOnly());
        QCOMPARE(child<QLineEdit>(d.get(), "storeLocation")->text(), env.stores.path());
        QVERIFY(child<QLabel>(d.get(), "launchRuleHelp")->text().contains("exactly one arm64 slice"));
        QVERIFY(child<QLabel>(d.get(), "storeHeadline")->text().startsWith("No private store yet"));
        QVERIFY(child<QPushButton>(d.get(), "copyStore")->isEnabled());
        const QString source = child<QLabel>(d.get(), "sourceLabel")->text();         // the store location is the only path on the page
        QVERIFY(source.contains("System/Library/dyld")); QVERIFY(!source.contains(env.volume.path())); QVERIFY(!source.contains(QFileInfo(env.volume.path()).canonicalFilePath()));
        QVERIFY(!child<QPushButton>(d.get(), "verifyStore")->isEnabled());      // nothing to verify yet
        QVERIFY(!child<QPushButton>(d.get(), "cancelJob")->isEnabled());
        QVERIFY(!child<QProgressBar>(d.get(), "copyProgress")->isVisible());
        // Darling wins by default: nothing is enabled, and enabling needs a ready store
        QVERIFY(!child<QCheckBox>(d.get(), "enableCache")->isChecked());
        QVERIFY(!child<QCheckBox>(d.get(), "enableCache")->isEnabled());
        QVERIFY(child<QCheckBox>(d.get(), "enableCache")->toolTip().contains("No ready store"));
        QVERIFY(child<QLabel>(d.get(), "prefixStatus")->text().startsWith("Off:"));
        for (auto *box : d->findChildren<QCheckBox *>()) if (box->text() == kLibz) { QVERIFY(!box->isChecked()); QVERIFY(!box->isEnabled()); }
        QVERIFY(child<QLabel>(d.get(), "appLine")->text().contains("Select an app"));
    }
    void withoutAVolumeOrPrefixThePageSaysWhy() {
        QTemporaryDir stores;
        DyldCacheDialog d({}, {}, stores.path(), {});
        d.show(); idle(&d);
        QVERIFY(child<QLabel>(&d, "sourceLabel")->text().contains("No mounted macOS volume"));
        QVERIFY(!child<QPushButton>(&d, "copyStore")->isEnabled());
        QVERIFY(!d.findChild<QWidget *>("prefixGroup")->isEnabled());
    }
    // A stalled FUSE mount blocks the probe in a syscall; the page must still close, and must not wait for it.
    void closingDuringAStuckVolumeReadDoesNotBlockOrTouchTheClosedPage() {
        Env env; env.makeSource();
        QSemaphore entered, release; std::atomic<bool> sawCancel{false};
        auto *d = new DyldCacheDialog(env.volume.path(), env.prefix.path(), env.stores.path(), {}, nullptr,
            [&](const QString &, const std::atomic<bool> &cancel, QString *, QString *, QString *error) {
                entered.release();
                while (!release.tryAcquire(1, 5)) if (cancel.load()) sawCancel = true;   // stuck, but looking at the flag
                *error = "late"; return false; });
        d->show();
        QVERIFY(entered.tryAcquire(1, kWaitMs));
        QVERIFY(d->isBusy()); QVERIFY(!child<QPushButton>(d, "cancelJob")->isEnabled());
        d->reject();
        QVERIFY(!d->isVisible());                                                   // closed although the read has not returned
        QTRY_VERIFY_WITH_TIMEOUT(sawCancel.load(), kWaitMs);                        // the read was told to stop by the close itself
        QElapsedTimer t; t.start();
        delete d;                                                                   // and destroying it does not wait for the read
        QVERIFY2(t.elapsed() < 2000, qPrintable(QString::number(t.elapsed())));
        release.release();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(kWaitMs));
        QTest::qWait(50);                                                           // a late result reaches no deleted page
    }
    void aLateVolumeReadIsDroppedWhenThePageWasClosedButNotDeleted() {
        Env env; env.makeSource();
        QSemaphore entered, release;
        DyldCacheDialog d(env.volume.path(), env.prefix.path(), env.stores.path(), {}, nullptr,
            [&](const QString &, const std::atomic<bool> &, QString *dir, QString *id, QString *) {
                entered.release(); release.acquire(); *dir = "/late"; *id = "arm64e-" + QString(32, 'a'); return true; });
        d.show(); QVERIFY(entered.tryAcquire(1, kWaitMs));
        d.reject(); release.release();
        QTRY_VERIFY_WITH_TIMEOUT(!d.isBusy(), kWaitMs);
        QCOMPARE(child<QLabel>(&d, "sourceLabel")->text(), QString("Reading the volume…"));     // the result changed nothing
        QVERIFY(!child<QPushButton>(&d, "copyStore")->isEnabled());
    }
    void aSourceProblemNamesNoPath() {
        Env env; QVERIFY(QDir().mkpath(env.volume.path() + "/System/Library/dyld"));       // a dyld directory without a cache in it
        std::unique_ptr<DyldCacheDialog> d(env.dialog()); idle(d.get());
        const QString text = child<QLabel>(d.get(), "sourceLabel")->text();
        QVERIFY(text.startsWith("Source unavailable")); QVERIFY2(!text.contains(env.volume.path()), qPrintable(text));
        QVERIFY(!child<QPushButton>(d.get(), "copyStore")->isEnabled());
    }
    void copyRunsInTheBackgroundAndEndsCurrent() {
        Env env; env.makeSource(1, 16384);
        std::unique_ptr<DyldCacheDialog> d(env.dialog({}, [] { return Env::fast(20, 4096); })); idle(d.get());
        auto *copy = child<QPushButton>(d.get(), "copyStore");
        QTimer ticker; int ticks = 0; ticker.start(5); QObject::connect(&ticker, &QTimer::timeout, [&] { ++ticks; });
        copy->click();
        QVERIFY(d->isBusy());                                                      // the click returned while the copy runs
        QVERIFY(!copy->isEnabled()); QVERIFY(child<QPushButton>(d.get(), "cancelJob")->isEnabled());
        idle(d.get());
        QVERIFY2(ticks > 3, "the event loop kept running during the copy");
        QCOMPARE(child<QLabel>(d.get(), "progressLabel")->text(), QString("Copy finished."));
        QVERIFY(child<QLabel>(d.get(), "storeHeadline")->text().contains("current"));
        QVERIFY(child<QLabel>(d.get(), "storeRows")->text().contains("3 files"));
        QVERIFY(child<QPushButton>(d.get(), "verifyStore")->isEnabled());
        QVERIFY(env.errors.isEmpty());
        const QString id = DyldCacheStore::listStores(env.stores.path()).value(0);
        QCOMPARE(DyldCacheStore::status(env.stores.path(), id).state, StoreState::Ready);
        child<QPushButton>(d.get(), "verifyStore")->click(); idle(d.get());
        QVERIFY(child<QLabel>(d.get(), "progressLabel")->text().startsWith("Verified")); QVERIFY(env.errors.isEmpty());
        // another macOS build on the volume makes the store stale, and the page says so
        QDir(env.volume.path() + "/System/Library/dyld").removeRecursively();
        env.makeSource(2);
        child<QPushButton>(d.get(), "rereadStore")->click(); idle(d.get());
        QVERIFY(child<QLabel>(d.get(), "storeHeadline")->text().startsWith("STALE"));
    }
    void theBarOnlyMovesForwardWhileSeveralStoresAreVerified() {
        Env env; env.makeSource(1, 3 << 20);
        std::unique_ptr<DyldCacheDialog> d(env.dialog()); idle(d.get());
        child<QPushButton>(d.get(), "copyStore")->click(); idle(d.get());
        QDir(env.volume.path() + "/System/Library/dyld").removeRecursively();
        env.makeSource(2, 3 << 20);                                                  // a second build, a second store
        child<QPushButton>(d.get(), "rereadStore")->click(); idle(d.get());
        child<QPushButton>(d.get(), "copyStore")->click(); idle(d.get());
        QCOMPARE(DyldCacheStore::listStores(env.stores.path()).size(), 2);
        QList<int> values;
        auto *bar = child<QProgressBar>(d.get(), "copyProgress");
        QObject::connect(bar, &QProgressBar::valueChanged, [&](int v) { values << v; });
        child<QPushButton>(d.get(), "verifyStore")->click(); idle(d.get());
        QVERIFY(child<QLabel>(d.get(), "progressLabel")->text().startsWith("Verified")); QVERIFY(env.errors.isEmpty());
        QVERIFY2(values.size() >= 4, qPrintable(QString::number(values.size())));
        for (int i = 1; i < values.size(); ++i) QVERIFY2(values[i] >= values[i - 1], qPrintable(QString("%1 after %2").arg(values[i]).arg(values[i - 1])));
        QVERIFY(values.last() > 500);                                                // the second store got the second half
    }
    void cancelStopsCleanlyAndCopyResumes() {
        Env env; env.makeSource(1, 6 << 20);
        QAtomicInt windows;
        auto options = [&windows] { auto o = Env::fast(0, 1 << 20); o.onWindow = [&windows](const QString &, qint64, qint64) { windows.fetchAndAddRelaxed(1); QThread::msleep(40); }; return o; };
        std::unique_ptr<DyldCacheDialog> d(env.dialog({}, options)); idle(d.get());
        child<QPushButton>(d.get(), "copyStore")->click();
        QTRY_VERIFY_WITH_TIMEOUT(windows.loadRelaxed() >= 2, kWaitMs);
        d->reject();                                                               // closing asks for a stop and keeps the page open
        QVERIFY(d->isVisible());
        idle(d.get());
        QVERIFY(child<QLabel>(d.get(), "progressLabel")->text().startsWith("Cancelled"));
        QVERIFY(child<QLabel>(d.get(), "storeHeadline")->text().startsWith("STALE"));
        QVERIFY(child<QLabel>(d.get(), "storeRows")->text().contains("Incomplete"));
        QVERIFY(env.errors.isEmpty());                                             // a cancel is not an error
        const int before = windows.loadRelaxed();
        child<QPushButton>(d.get(), "copyStore")->click(); idle(d.get());          // resumes instead of starting over
        QVERIFY(child<QLabel>(d.get(), "storeHeadline")->text().contains("current"));
        QVERIFY2(windows.loadRelaxed() - before < 3 * 6, "the resumed copy did not read everything again");
        d->reject(); QVERIFY(!d->isVisible());
    }
    void aFailedCopyShowsTheCoresMessageWithoutAPath() {
        Env env; env.makeSource(1, 8192);
        QVERIFY(QFile::remove(env.volume.path() + "/System/Library/dyld/dyld_shared_cache_arm64e.02"));
        std::unique_ptr<DyldCacheDialog> d(env.dialog()); idle(d.get());
        child<QPushButton>(d.get(), "copyStore")->click(); idle(d.get());
        QVERIFY(child<QLabel>(d.get(), "progressLabel")->text().startsWith("Copy failed"));
        QCOMPARE(env.errors.size(), 1);
        QVERIFY(!env.errors[0].second.isEmpty()); QVERIFY(!env.errors[0].second.contains(env.volume.path()));
        QVERIFY(DyldCacheStore::status(env.stores.path(), DyldCacheStore::listStores(env.stores.path()).value(0)).state != StoreState::Ready);
    }
    void thePrefixSectionWritesThroughTheConfigWriter() {
        Env env; env.makeSource();
        const QString id = makeFakeStore(env.stores.path(), "arm64e", 1, {".01", ".02"});
        std::unique_ptr<DyldCacheDialog> d(env.dialog()); idle(d.get());
        auto *enable = child<QCheckBox>(d.get(), "enableCache"); auto *save = child<QPushButton>(d.get(), "savePrefix");
        QVERIFY(enable->isEnabled()); QVERIFY(!enable->isChecked()); QVERIFY(!save->isEnabled());
        QCOMPARE(child<QComboBox>(d.get(), "storeChoice")->currentData().toString(), id);
        QVERIFY(!QFileInfo::exists(DyldCacheConfigFile::configPath(env.prefix.path())));   // nothing written until asked
        enable->setChecked(true);
        QVERIFY(child<QLabel>(d.get(), "prefixStatus")->text().contains("no library is selected"));
        QCheckBox *libz = nullptr;
        for (auto *box : d->findChildren<QCheckBox *>()) if (box->text() == kLibz) libz = box;
        QVERIFY(libz && libz->isEnabled()); libz->setChecked(true);
        QVERIFY(child<QLabel>(d.get(), "prefixStatus")->text().startsWith("Active: 1 library"));
        QVERIFY(save->isEnabled());
        save->click();
        QVERIFY(env.errors.isEmpty());
        DyldCacheConfig c; QString err; QVERIFY2(DyldCacheConfigFile::load(env.prefix.path(), &c, &err), qPrintable(err));
        QVERIFY(c.mode == DyldCacheMode::AllowList); QCOMPARE(c.store, id); QCOMPARE(c.served, QStringList{kLibz});
        QVERIFY(QFileInfo::exists(DyldCacheConfigFile::policyPath(env.prefix.path())));
        QVERIFY(!save->isEnabled());
        enable->setChecked(false); save->click();                                             // off again keeps the selection, drops the policy
        QVERIFY(DyldCacheConfigFile::load(env.prefix.path(), &c, &err)); QVERIFY(c.mode == DyldCacheMode::Off); QCOMPARE(c.served, QStringList{kLibz});
        QVERIFY(!QFileInfo::exists(DyldCacheConfigFile::policyPath(env.prefix.path())));
        // a reopened page shows the saved state
        std::unique_ptr<DyldCacheDialog> again(env.dialog()); idle(again.get());
        QVERIFY(!child<QCheckBox>(again.get(), "enableCache")->isChecked());
    }
    void aStoreThatIsGoneBlocksEnablingButNeverDisabling() {
        Env env; env.makeSource();
        const QString gone = DyldCacheStore::storeId("arm64e", QString(32, 'e'));
        DyldCacheConfig c; c.mode = DyldCacheMode::AllowList; c.store = gone; c.served = {kLibz};
        QString err; QVERIFY2(DyldCacheConfigFile::save(env.prefix.path(), c, &err), qPrintable(err));
        std::unique_ptr<DyldCacheDialog> d(env.dialog()); idle(d.get());
        auto *enable = child<QCheckBox>(d.get(), "enableCache");
        QVERIFY(enable->isChecked()); QVERIFY(enable->isEnabled());                         // can still be unticked
        QVERIFY(child<QLabel>(d.get(), "prefixStatus")->text().startsWith("Not active"));
        QVERIFY(child<QLabel>(d.get(), "prefixStatus")->text().contains("refuse to start"));
        enable->setChecked(false); child<QPushButton>(d.get(), "savePrefix")->click();
        QVERIFY(env.errors.isEmpty());
        QVERIFY(DyldCacheConfigFile::load(env.prefix.path(), &c, &err)); QVERIFY(c.mode == DyldCacheMode::Off);
        QVERIFY(!enable->isEnabled());                                                       // now off and still no ready store
    }
    void savingNeverPinsAnEnabledPrefixToAStoreThatIsNotReady() {
        Env env; env.makeSource();
        const QString a = makeFakeStore(env.stores.path(), "arm64e", 1, {});
        const QString b = makeFakeStore(env.stores.path(), "arm64e", 2, {});
        QVERIFY(QFile::remove(env.stores.path() + "/" + b + "/store.json"));               // B is a partial copy
        DyldCacheConfig c; c.mode = DyldCacheMode::Off; c.store = b; c.served = {};
        QString err; QVERIFY2(DyldCacheConfigFile::save(env.prefix.path(), c, &err), qPrintable(err));
        const QByteArray before = [&] { QFile f(DyldCacheConfigFile::configPath(env.prefix.path())); f.open(QIODevice::ReadOnly); return f.readAll(); }();
        std::unique_ptr<DyldCacheDialog> d(env.dialog()); idle(d.get());
        auto *combo = child<QComboBox>(d.get(), "storeChoice"); auto *save = child<QPushButton>(d.get(), "savePrefix");
        combo->setCurrentIndex(combo->findData(a));
        child<QCheckBox>(d.get(), "enableCache")->setChecked(true);
        for (auto *box : d->findChildren<QCheckBox *>()) if (box->text() == kLibz) box->setChecked(true);
        QVERIFY(save->isEnabled());                                                         // A is ready
        combo->setCurrentIndex(combo->findData(b));                                         // switch to the partial one
        QVERIFY(child<QLabel>(d.get(), "prefixStatus")->text().startsWith("Not active"));
        QVERIFY(!save->isEnabled());
        save->click();                                                                      // a click on a disabled button does nothing
        QVERIFY(env.errors.isEmpty());
        QFile f(DyldCacheConfigFile::configPath(env.prefix.path())); QVERIFY(f.open(QIODevice::ReadOnly)); QCOMPARE(f.readAll(), before);
    }
    void saveChecksTheStoreAgainAtSaveTime() {
        Env env; env.makeSource();
        const QString a = makeFakeStore(env.stores.path(), "arm64e", 1, {});
        std::unique_ptr<DyldCacheDialog> d(env.dialog()); idle(d.get());
        child<QCheckBox>(d.get(), "enableCache")->setChecked(true);
        for (auto *box : d->findChildren<QCheckBox *>()) if (box->text() == kLibz) box->setChecked(true);
        auto *save = child<QPushButton>(d.get(), "savePrefix"); QVERIFY(save->isEnabled());
        QVERIFY(QFile::remove(env.stores.path() + "/" + a + "/store.json"));               // the store changes after the box was ticked
        save->click();
        QCOMPARE(env.errors.size(), 1); QVERIFY(env.errors[0].second.contains("is not ready"));
        QVERIFY(!QFileInfo::exists(DyldCacheConfigFile::configPath(env.prefix.path())));
        QVERIFY(!QFileInfo::exists(DyldCacheConfigFile::policyPath(env.prefix.path())));
        QVERIFY(!save->isEnabled());                                                        // the page re-evaluated
    }
    void anUnreadableConfigIsReportedAndSavingReplacesIt() {
        Env env; env.makeSource();
        makeFakeStore(env.stores.path(), "arm64e", 1, {});
        QDir().mkpath(env.prefix.path() + "/.darling-launcher");
        { QFile f(DyldCacheConfigFile::configPath(env.prefix.path())); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("{not json"); }
        std::unique_ptr<DyldCacheDialog> d(env.dialog()); idle(d.get());
        QVERIFY(child<QLabel>(d.get(), "prefixStatus")->text().contains("cannot be read"));
        auto *save = child<QPushButton>(d.get(), "savePrefix"); QVERIFY(save->isEnabled());
        save->click(); QVERIFY(env.errors.isEmpty());
        DyldCacheConfig c; QString err; QVERIFY2(DyldCacheConfigFile::load(env.prefix.path(), &c, &err), qPrintable(err));
        QVERIFY(c.mode == DyldCacheMode::Off);
    }
    void theAppLineAppliesTheArm64eRule() {
        Env env; env.makeSource();
        const QString id = makeFakeStore(env.stores.path(), "arm64e", 1, {});
        DyldCacheConfig c; c.mode = DyldCacheMode::AllowList; c.store = id; c.served = {kLibz};
        QString err; QVERIFY2(DyldCacheConfigFile::save(env.prefix.path(), c, &err), qPrintable(err));
        QVERIFY(QDir().mkpath(env.prefix.path() + "/bin"));
        { QFile f(env.prefix.path() + "/bin/plain"); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(thin(0, {})); }
        { QFile f(env.prefix.path() + "/bin/pac"); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(thin(2, {})); }
        { std::unique_ptr<DyldCacheDialog> d(env.dialog({"Plain", "bin/plain"})); idle(d.get());
          QVERIFY(child<QLabel>(d.get(), "appLine")->text().contains("non-arm64e")); }
        { std::unique_ptr<DyldCacheDialog> d(env.dialog({"Pac", "bin/pac"})); idle(d.get());
          QVERIFY(child<QLabel>(d.get(), "appLine")->text().contains("gets the cache")); }
    }
};
QTEST_MAIN(DyldCacheDialogTest)
#include "dyldcachedialog_test.moc"
