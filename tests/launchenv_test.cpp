// SPDX-License-Identifier: GPL-3.0-or-later
#include "dyldcache_fixture.h"
#include "dyldcacheconfig.h"
#include "launchenv.h"
#include "core.h"
#include <QProcessEnvironment>
#include <QSettings>
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
    DyldCacheConfig config() const { DyldCacheConfig c; c.enabled = true; c.mode = DyldCacheMode::AllowList; c.store = id; c.served = {kLibz}; return c; }
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
        for (int v = 0; v < 3; ++v) {
            Fx fx; DyldCacheConfig c = fx.config(); if (v == 0) c.enabled = false; else if (v == 1) c.mode = DyldCacheMode::Off; else c.served.clear();
            QVERIFY(fx.save(c)); QProcessEnvironment e; QString err; QVERIFY2(fx.env(&e, &err), qPrintable(err)); QCOMPARE(Fx::dyldKeys(e), QStringList());
        }
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
