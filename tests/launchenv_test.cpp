// SPDX-License-Identifier: GPL-3.0-or-later
#include "dyldcache_fixture.h"
#include "dyldcacheconfig.h"
#include "launchenv.h"
#include "macho_fixture.h"
#include "core.h"
#include <QProcessEnvironment>
#include <QSettings>
#include <sys/stat.h>
#include <QTemporaryDir>

static const QString kLibz = "/usr/lib/libz.1.dylib";

struct Fx {
    QTemporaryDir prefix, proc, stores;
    QString id;
    QProcessEnvironment base;
    Fx() {
        id = makeFakeStore(stores.path(), "arm64e", 1, {".01"});
        base.insert("PATH", "/usr/bin"); base.insert("DPREFIX", "keepme");
        base.insert("DYLD_INSERT_LIBRARIES", "/evil.dylib"); base.insert("DYLD_FOO", "1");
        base.insert("DYLD_SHARED_CACHE_SPLIT_DIR", "/volumes/evil"); base.insert("DYLD_SHARED_CACHE_SPLIT_POLICY", "/evil.policy");
    }
    DyldCacheConfig config() const { DyldCacheConfig c; c.mode = DyldCacheMode::AllowList; c.store = id; c.served = {kLibz}; return c; }
    bool save(const DyldCacheConfig &c) {
        PrefixState::ProcFs p; p.procRoot = proc.path(); p.socketMayBeLive = [](const QString &) { return false; };
        QString err; return DyldCacheConfigFile::save(prefix.path(), c, &err, p);
    }
    bool env(QProcessEnvironment *e, QString *err) { *e = base; return LaunchEnv::guestEnvironment(e, prefix.path(), stores.path(), err); }
    static QStringList dyldKeys(const QProcessEnvironment &e) { QStringList k; for (const auto &n : e.keys()) if (n.startsWith("DYLD_")) k << n; k.sort(); return k; }
};

class LaunchEnvTest : public QObject {
    Q_OBJECT
private slots:
    void withoutAConfigEveryDyldVariableIsStripped() {
        Fx fx; QProcessEnvironment e; QString err;
        QVERIFY2(fx.env(&e, &err), qPrintable(err));
        QCOMPARE(Fx::dyldKeys(e), QStringList()); QCOMPARE(e.value("PATH"), QString("/usr/bin")); QCOMPARE(e.value("DPREFIX"), QString("keepme"));
        e = fx.base; QVERIFY(LaunchEnv::guestEnvironment(&e, QString(), fx.stores.path(), &err)); QCOMPARE(Fx::dyldKeys(e), QStringList());
    }
    void enabledSetsExactlyTheTwoVariables() {
        Fx fx; QVERIFY(fx.save(fx.config())); QProcessEnvironment e; QString err;
        QVERIFY2(fx.env(&e, &err), qPrintable(err));
        QCOMPARE(Fx::dyldKeys(e), QStringList({"DYLD_SHARED_CACHE_SPLIT_DIR", "DYLD_SHARED_CACHE_SPLIT_POLICY"}));
        QCOMPARE(e.value("DYLD_SHARED_CACHE_SPLIT_DIR"), QString("/Volumes/SystemRoot") + QFileInfo(fx.stores.path() + "/" + fx.id).canonicalFilePath());
        QCOMPARE(e.value("DYLD_SHARED_CACHE_SPLIT_POLICY"), QString("/.darling-launcher/dyld-cache.policy"));
        QCOMPARE(e.value("PATH"), QString("/usr/bin"));
    }
    void offModesAddNothing() {
        for (int v = 0; v < 2; ++v) {
            Fx fx; DyldCacheConfig c = fx.config(); if (v == 0) c.mode = DyldCacheMode::Off; else c.served.clear();
            QVERIFY(fx.save(c)); QProcessEnvironment e; QString err; QVERIFY2(fx.env(&e, &err), qPrintable(err)); QCOMPARE(Fx::dyldKeys(e), QStringList());
        }
    }
    // The guest runs as the same user and could otherwise be handed a cache another user can swap.
    void refusesAStoreThatIsOpenToOthersOrSymlinked() {
        { Fx fx; QVERIFY(fx.save(fx.config())); QProcessEnvironment e; QString err; QVERIFY2(fx.env(&e, &err), qPrintable(err)); }
        { Fx fx; QVERIFY(fx.save(fx.config())); QVERIFY(::chmod(fx.stores.path().toUtf8().constData(), 0755) == 0);
          QProcessEnvironment e; QString err; QVERIFY(!fx.env(&e, &err)); QVERIFY(err.contains("0700")); QVERIFY(e == fx.base); }
        { Fx fx; QVERIFY(fx.save(fx.config())); QVERIFY(::chmod((fx.stores.path() + "/" + fx.id).toUtf8().constData(), 0750) == 0);
          QProcessEnvironment e; QString err; QVERIFY(!fx.env(&e, &err)); QVERIFY(err.contains("0700")); }
        { Fx fx; QVERIFY(fx.save(fx.config())); QTemporaryDir links; QVERIFY(QFile::link(fx.stores.path(), links.path() + "/l"));
          QProcessEnvironment e = fx.base; QString err; QVERIFY(!LaunchEnv::guestEnvironment(&e, fx.prefix.path(), links.path() + "/l", &err)); QVERIFY(err.contains("symbolic link")); }
    }
    // forRun is the one decision point of the window: kind of run, then the app's own architecture.
    static QByteArray fatOf(const QList<QPair<quint32, quint32>> &cpuSubs) {
        QByteArray b; put32(b, 0xcafebabe, true); put32(b, quint32(cpuSubs.size()), true);
        for (const auto &c : cpuSubs) { put32(b, c.first, true); put32(b, c.second, true); put32(b, 0x4000, true); put32(b, 0x100, true); put32(b, 14, true); }
        return b;
    }
    void forRunGivesTheCacheToArm64eAppLaunchesOnly() {
        struct Case { const char *name; QByteArray exe; bool cache; const char *note; };
        const quint32 arm64 = 0x0100000c, x86 = 0x01000007;
        const QList<Case> cases = {
            {"thin arm64e", thin(2, {}), true, ""},
            {"thin arm64e with pac abi bits", thin(0x80000002u, {}), true, ""},
            {"thin arm64", thin(0, {}), false, "non-arm64e"},
            {"thin x86_64", thinWith(3, {}, x86), false, "not an arm64"},
            {"fat arm64 and arm64e", fatOf({{arm64, 0}, {arm64, 2}}), false, "non-arm64e"},
            {"fat arm64e and x86_64", fatOf({{arm64, 2}, {x86, 3}}), true, ""},
            {"not a Mach-O", QByteArray("#!/bin/sh\n"), false, "could not be determined"},
            {"truncated fat table", fatOf({{arm64, 2}}).left(12), false, "could not be determined"},
        };
        for (const Case &c : cases) {
            Fx fx; QVERIFY(fx.save(fx.config()));
            const QString rel = "Applications/X.app/Contents/MacOS/X";
            QDir().mkpath(fx.prefix.path() + "/Applications/X.app/Contents/MacOS");
            { QFile f(fx.prefix.path() + "/" + rel); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(c.exe); }
            QProcessEnvironment e = fx.base; QString err, note;
            QVERIFY2(LaunchEnv::forRun(LaunchEnv::RunKind::App, &e, fx.prefix.path(), fx.stores.path(), rel, &err, &note), c.name);
            QCOMPARE(e.contains("DYLD_SHARED_CACHE_SPLIT_DIR"), c.cache);
            QCOMPARE(Fx::dyldKeys(e).size(), c.cache ? 2 : 0);
            QVERIFY2(note.contains(c.note), qPrintable(QString(c.name) + ": " + note)); if (c.cache) QVERIFY(note.isEmpty());
            QVERIFY(!note.contains(fx.prefix.path()));
        }
        Fx fx; QVERIFY(fx.save(fx.config())); QString err, note; QProcessEnvironment e = fx.base;   // no executable on disk
        QVERIFY(LaunchEnv::forRun(LaunchEnv::RunKind::App, &e, fx.prefix.path(), fx.stores.path(), "Applications/Gone.app/Contents/MacOS/Gone", &err, &note));
        QCOMPARE(Fx::dyldKeys(e).size(), 0); QVERIFY(note.contains("could not be determined"));
    }
    // The settings UI asks the same rule without building an environment.
    void appRunsAsArm64eIsTheRuleForRunUses() {
        QTemporaryDir prefix; QDir().mkpath(prefix.path() + "/bin");
        const auto put = [&](const char *name, const QByteArray &data) { QFile f(prefix.path() + "/bin/" + name); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(data); };
        put("e", thin(2, {})); put("p", thin(0, {})); put("fat", fatOf({{0x0100000c, 0}, {0x0100000c, 2}}));
        QString note;
        QVERIFY(LaunchEnv::appRunsAsArm64e(prefix.path(), "bin/e", &note)); QVERIFY(note.isEmpty());
        QVERIFY(!LaunchEnv::appRunsAsArm64e(prefix.path(), "bin/p", &note)); QVERIFY(note.contains("non-arm64e"));
        QVERIFY(!LaunchEnv::appRunsAsArm64e(prefix.path(), "bin/fat", &note)); QVERIFY(note.contains("non-arm64e"));
        QVERIFY(!LaunchEnv::appRunsAsArm64e(prefix.path(), "bin/gone", &note)); QVERIFY(note.contains("could not be determined"));
        QVERIFY(!LaunchEnv::appRunsAsArm64e(prefix.path(), QString(), &note)); QVERIFY(note.contains("no executable"));
        QVERIFY(note.endsWith("not used for this launch."));
    }
    void forRunMaintenanceNeverGetsTheCacheAndAnAppWithoutTheCacheGetsNoNote() {
        Fx fx; QVERIFY(fx.save(fx.config()));
        const QString rel = "Applications/X.app/Contents/MacOS/X";
        QDir().mkpath(fx.prefix.path() + "/Applications/X.app/Contents/MacOS");
        { QFile f(fx.prefix.path() + "/" + rel); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(thin(2, {})); }
        QProcessEnvironment e = fx.base; QString err, note;
        QVERIFY(LaunchEnv::forRun(LaunchEnv::RunKind::Maintenance, &e, fx.prefix.path(), fx.stores.path(), rel, &err, &note));
        QCOMPARE(Fx::dyldKeys(e).size(), 0); QVERIFY(note.isEmpty()); QCOMPARE(e.value("PATH"), QString("/usr/bin"));
        Fx plain; e = plain.base;                       // a prefix that did not ask for the cache: stripped, nothing to explain
        QVERIFY(LaunchEnv::forRun(LaunchEnv::RunKind::App, &e, plain.prefix.path(), plain.stores.path(), rel, &err, &note));
        QCOMPARE(Fx::dyldKeys(e).size(), 0); QVERIFY(note.isEmpty());
        e = fx.base;                                    // a broken store still fails an app launch loudly
        QVERIFY(QFile::remove(fx.stores.path() + "/" + fx.id + "/store.json"));
        QVERIFY(!LaunchEnv::forRun(LaunchEnv::RunKind::App, &e, fx.prefix.path(), fx.stores.path(), rel, &err, &note)); QVERIFY(err.contains("not usable"));
    }
    void failsLoudAndLeavesTheEnvironmentAloneWhenTheStoreIsNotUsable() {
        auto expectRefusal = [](Fx &fx, const QString &needle) {
            QProcessEnvironment e; QString err; e = fx.base;
            QVERIFY2(!LaunchEnv::guestEnvironment(&e, fx.prefix.path(), fx.stores.path(), &err), qPrintable(needle));
            QVERIFY2(err.contains(needle), qPrintable(err)); QVERIFY(e == fx.base);
            QVERIFY(!err.contains(fx.stores.path())); QVERIFY(!err.contains(fx.prefix.path()));
        };
        { Fx fx; QVERIFY(fx.save(fx.config())); QDir(fx.stores.path() + "/" + fx.id).removeRecursively(); expectRefusal(fx, "not usable"); }          // scratch fixture only
        { Fx fx; QVERIFY(fx.save(fx.config())); QVERIFY(QFile::remove(fx.stores.path() + "/" + fx.id + "/store.json")); expectRefusal(fx, "not usable"); }
        { Fx fx; QVERIFY(fx.save(fx.config())); QFile f(fx.stores.path() + "/" + fx.id + "/dyld_shared_cache_arm64e.01"); QVERIFY(f.open(QIODevice::Append)); f.write("x"); f.close(); expectRefusal(fx, "not usable"); }
        { Fx fx; QVERIFY(fx.save(fx.config())); QVERIFY(QFile::remove(DyldCacheConfigFile::policyPath(fx.prefix.path()))); expectRefusal(fx, "policy"); }
        { Fx fx; QVERIFY(fx.save(fx.config())); QFile f(DyldCacheConfigFile::policyPath(fx.prefix.path())); QVERIFY(f.open(QIODevice::Append)); f.write("/usr/lib/libfoo.dylib\n"); f.close(); expectRefusal(fx, "policy"); }
        { Fx fx; QVERIFY(fx.save(fx.config())); QFile f(DyldCacheConfigFile::configPath(fx.prefix.path())); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate)); f.write("garbage"); f.close(); expectRefusal(fx, "dyld-cache.json"); }
    }
    // Maintenance runs strip DYLD_* and never carry the cache variables, even for a prefix that asked for the cache:
    // an arm64 tool would otherwise depend on a cache file for its own architecture.
    void maintenanceRunsNeverCarryTheCacheVariables() {
        Fx fx; QTemporaryDir cfg, tools; QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, cfg.path());
        QSettings("cristim", "darling-launcher").setValue("dyldCacheRoot", fx.stores.path());
        const QString log = tools.path() + "/env.log", script = tools.path() + "/darling";
        { QFile f(script); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("#!/bin/sh\nenv | grep '^DYLD_' | sort >> \"$ENVLOG\"\necho --- >> \"$ENVLOG\"\nexit 0\n"); f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner); }
        qputenv("ENVLOG", log.toUtf8()); qputenv("DYLD_INSERT_LIBRARIES", "/evil.dylib");
        QVERIFY(fx.save(fx.config()));
        const auto run = [&](const std::function<void()> &fn) { QFile::remove(log); fn(); QFile f(log); return f.open(QIODevice::ReadOnly) ? QString::fromLocal8Bit(f.readAll()) : QString(); };
        const QString check = run([&] { LauncherCore::checkPrefix(script, QString(), fx.prefix.path()); });
        const QString repair = run([&] { LauncherCore::repairPrefix(script, QString(), fx.prefix.path()); });
        for (const QString &out : {check, repair}) { QVERIFY(out.contains("---")); QVERIFY2(!out.contains("DYLD_"), qPrintable(out)); }
        // even with the store damaged the maintenance runs still start: they do not depend on the cache
        QVERIFY(QFile::remove(fx.stores.path() + "/" + fx.id + "/store.json"));
        for (const auto &c : LauncherCore::checkPrefix(script, QString(), fx.prefix.path())) QVERIFY2(!c.detail.contains("not usable"), qPrintable(c.detail));
        qunsetenv("DYLD_INSERT_LIBRARIES");
    }
};
QTEST_APPLESS_MAIN(LaunchEnvTest)
#include "launchenv_test.moc"
