// SPDX-License-Identifier: GPL-3.0-or-later
// No pkexec, no mount and no privileged helper runs here: the step is a fake that records what it was asked to do.
#include "automount.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace AutoMount;

struct FakeStep : Step {
    QList<StepRequest> asked;
    int exitCode = 0; bool crash = false;
    std::function<void(const StepRequest &)> effect;
    StepResult run(const StepRequest &request) override {
        asked << request;
        if (effect) effect(request);
        return {exitCode, crash, "helper output"};
    }
};
static void touchDir(const QString &path) { QVERIFY(QDir().mkpath(path)); }
static const MacPartition kPart{"/dev/nvme0n1p2", "apfs", "Container", false, {}, 245000000000LL};

struct World {
    QTemporaryDir dir;
    QList<SourceMount> mounts;
    QString app() const { return dir.path() + "/system"; }
    QString cache() const { return dir.path() + "/cryptex"; }
    void mountApp() { touchDir(app() + "/System/Applications"); touchDir(app() + "/System/Library"); mounts << SourceMount{app(), "dev", "apfs", true}; }
    void mountCache() { touchDir(cache() + "/System/Library/dyld"); mounts << SourceMount{cache(), "/dev/loop-cryptex", "fuse", true}; } // a device name that makes it a source candidate, so only the apps check keeps it from counting as the app volume
    Hooks hooks(bool consent, int *consentCalls = nullptr, QStringList *shown = nullptr, int *sleeps = nullptr) {
        Hooks h;
        h.mounts = [this] { return mounts; };
        h.consent = [=](const QString &text) { if (consentCalls) ++*consentCalls; if (shown) shown->append(text); return consent; };
        h.sleep = [=](int) { if (sleeps) ++*sleeps; };
        return h;
    }
};

class AutoMountTest : public QObject {
    Q_OBJECT
private slots:
    void needsFollowWhatIsMounted() {
        World w;
        QVERIFY(needed(w.mounts, false).appVolume); QVERIFY(!needed(w.mounts, false).cacheVolume);
        QVERIFY(needed(w.mounts, true).cacheVolume);
        w.mountCache();                                   // the cryptex image alone has no apps: the app volume is still missing
        QVERIFY(needed(w.mounts, true).appVolume); QVERIFY(!needed(w.mounts, true).cacheVolume);
        w.mountApp();
        QVERIFY(!needed(w.mounts, true).any());
    }
    void alreadyMountedNeverAsksAndNeverRunsAHelper() {
        World w; w.mountApp(); FakeStep step; int asks = 0;
        const Outcome o = ensureMounted(false, kPart, step, w.hooks(true, &asks));
        QCOMPARE(int(o.state), int(Outcome::State::AlreadyMounted)); QCOMPARE(asks, 0); QVERIFY(step.asked.isEmpty());
    }
    void theExplanationSaysWhyWhatAndWhatNot() {
        const QString text = explanation({true, true}, kPart);
        for (const QString &needle : {"administrator rights", "raw disk device by root", "/dev/nvme0n1p2", "245.0 GB", "Preboot and System volumes", "cryptex image", "read-only",
                                      "does not write to the macOS partition", "does not change the partition table", "copies nothing yet", "separate preview and your confirmation",
                                      "until you reboot or unmount them", "authentication dialog (polkit)", "never sees or stores your password"})
            QVERIFY2(text.contains(needle), qPrintable(needle));
        QVERIFY(!text.contains("password:")); // nothing asks for one
        QVERIFY(explanation({false, true}, kPart).contains("Preboot volume and the macOS cryptex image"));
        QVERIFY(QString(kPolkitMessage).contains("administrator rights") && QString(kPolkitMessage).contains("read-only"));
    }
    void declineStopsCleanlyWithTheManualWayAndNoHelper() {
        World w; FakeStep step; int asks = 0; QStringList shown;
        const Outcome o = ensureMounted(false, kPart, step, w.hooks(false, &asks, &shown));
        QCOMPARE(int(o.state), int(Outcome::State::Cancelled)); QCOMPARE(asks, 1); QVERIFY(step.asked.isEmpty());
        QVERIFY(o.message.contains("Nothing was mounted")); QVERIFY(o.message.contains("apfs-fuse")); QVERIFY(shown.first().contains("administrator rights"));
    }
    void consentComesFirstThenOneStepPerMissingPart() {
        World w; FakeStep step; int asks = 0;
        step.effect = [&](const StepRequest &r) { if (r.kind == Kind::AppVolumes) w.mountApp(); else w.mountCache(); };
        const Outcome o = ensureMounted(true, kPart, step, w.hooks(true, &asks));
        QCOMPARE(int(o.state), int(Outcome::State::Mounted)); QCOMPARE(asks, 1);
        QCOMPARE(step.asked.size(), 2); QCOMPARE(int(step.asked[0].kind), int(Kind::AppVolumes)); QCOMPARE(int(step.asked[1].kind), int(Kind::CacheVolume));
        QCOMPARE(step.asked[0].device, QString("/dev/nvme0n1p2")); QCOMPARE(o.stepsRun, (QStringList{"app volumes", "cache volume"}));
    }
    void authorizationOutcomesAreNamedAndNothingIsRetried() {
        World w; FakeStep step; step.exitCode = 126;
        Outcome o = ensureMounted(false, kPart, step, w.hooks(true));
        QCOMPARE(int(o.state), int(Outcome::State::Cancelled)); QVERIFY(o.message.contains("cancelled")); QCOMPARE(step.asked.size(), 1);
        step.exitCode = 127; step.asked.clear();
        o = ensureMounted(false, kPart, step, w.hooks(true));
        QCOMPARE(int(o.state), int(Outcome::State::Failed)); QVERIFY(o.message.contains("denied")); QCOMPARE(step.asked.size(), 1);
        step.exitCode = 0; step.crash = true;
        o = ensureMounted(false, kPart, step, w.hooks(true)); QVERIFY(o.message.contains("crashed"));
    }
    void aHelperThatMountedNothingIsAFailureAfterWaiting() {
        World w; FakeStep step; int sleeps = 0;
        Hooks h = w.hooks(true, nullptr, nullptr, &sleeps); h.checks = 4;
        const Outcome o = ensureMounted(false, kPart, step, h);
        QCOMPARE(int(o.state), int(Outcome::State::Failed)); QCOMPARE(sleeps, 4); QVERIFY(o.message.contains("not readable yet"));
    }
    void aVolumeThatAppearsLateIsWaitedFor() {
        World w; FakeStep step; int sleeps = 0;
        Hooks h = w.hooks(true, nullptr, nullptr, &sleeps);
        h.sleep = [&](int) { if (++sleeps == 3) w.mountApp(); }; // Preboot was not ready at the first looks
        const Outcome o = ensureMounted(false, kPart, step, h);
        QCOMPARE(int(o.state), int(Outcome::State::Mounted)); QCOMPARE(sleeps, 3);
    }
    void noPartitionMeansNothingRuns() {
        World w; FakeStep step; int asks = 0;
        const Outcome o = ensureMounted(false, MacPartition{}, step, w.hooks(true, &asks));
        QCOMPARE(int(o.state), int(Outcome::State::Failed)); QCOMPARE(asks, 0); QVERIFY(step.asked.isEmpty());
        const QList<MacPartition> parts = apfsPartitions({{"/dev/a", "apfs", "", false, {}, 1}, {"/dev/b", "ext4", "", false, {}, 1}, {"/dev/c", "apfs", "", true, {}, 1}});
        QCOMPARE(parts.size(), 2);
    }
    void partitionSizesParseAsNumbersOrStrings() {
        const QByteArray json = R"({"blockdevices":[{"path":"/dev/a","type":"part","fstype":"apfs","label":"x","mountpoints":[null],"size":2000},{"path":"/dev/b","type":"part","fstype":"apfs","size":"3000"},{"path":"/dev/c","type":"part","fstype":"apfs"}]})";
        QString error; const auto parts = LauncherMount::parsePartitions(json, &error);
        QCOMPARE(parts.size(), 3); QCOMPARE(parts[0].sizeBytes, 2000LL); QCOMPARE(parts[1].sizeBytes, 3000LL); QCOMPARE(parts[2].sizeBytes, -1LL);
    }
    void theHelperCommandsAreFixedAndValidated() {
        QTemporaryDir dir; QFile script(dir.path() + "/mount-macos.py"), cryptex(dir.path() + "/mount-cryptex.py");
        QVERIFY(script.open(QIODevice::WriteOnly)); QVERIFY(cryptex.open(QIODevice::WriteOnly));
        const Tools tools{"/usr/bin/true", script.fileName(), cryptex.fileName(), "/usr/bin/true", dir.path() + "/mounts"};
        MountCommand c; QString error;
        QVERIFY2(commandFor({Kind::AppVolumes, "/dev/nvme0n1p2"}, tools, &c, &error), qPrintable(error));
        QCOMPARE(c.program, QString("/usr/bin/true")); QCOMPARE(c.arguments, (QStringList{"-I", script.fileName(), "--device=/dev/nvme0n1p2"}));
        MountCommand expected;
        QVERIFY(LauncherMount::makeCryptexCommand("/usr/bin/true", cryptex.fileName(), tools.mountRoot, "/usr/bin/true", "/dev/nvme0n1p2", &expected, &error));
        QVERIFY(commandFor({Kind::CacheVolume, "/dev/nvme0n1p2"}, tools, &c, &error));
        QCOMPARE(c.program, expected.program); QCOMPARE(c.arguments, expected.arguments);
        for (const QString &bad : {"/dev/../etc/passwd", "/dev/sda1; rm -rf /", "nvme0n1p2", "/tmp/x", ""}) {
            c = {"untouched", {}};
            QVERIFY2(!commandFor({Kind::AppVolumes, bad}, tools, &c, &error), qPrintable(bad)); QVERIFY(!commandFor({Kind::CacheVolume, bad}, tools, &c, &error));
            QCOMPARE(c.program, QString("untouched"));
        }
        Tools none = tools; none.python.clear();
        QVERIFY(!commandFor({Kind::AppVolumes, "/dev/a"}, none, &c, &error)); QVERIFY(error.contains("Python"));
    }
};
QTEST_GUILESS_MAIN(AutoMountTest)
#include "automount_test.moc"
