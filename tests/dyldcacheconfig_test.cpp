// SPDX-License-Identifier: GPL-3.0-or-later
#include "dyldcacheconfig.h"
#include "dyldcachestore.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <sys/stat.h>

using namespace DyldCacheConfigFile;

static const QString kLibz = "/usr/lib/libz.1.dylib";
static const QString kStore = "arm64e-" + QString(32, 'a');
static QByteArray slurp(const QString &p) { QFile f(p); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }
static int modeOf(const QString &p) { struct stat st; return ::stat(p.toUtf8().constData(), &st) == 0 ? int(st.st_mode & 0777) : -1; }
static void put(const QString &path, const QByteArray &data) { QDir().mkpath(QFileInfo(path).absolutePath()); QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(data); }

struct Fx {
    QTemporaryDir prefix, proc, evil;
    PrefixState::ProcFs procFs;
    Fx() { procFs.procRoot = proc.path(); procFs.socketMayBeLive = [](const QString &) { return false; }; }
    bool save(const DyldCacheConfig &c, QString *err = nullptr) { QString e; return DyldCacheConfigFile::save(prefix.path(), c, err ? err : &e, procFs); }
    static DyldCacheConfig on() { DyldCacheConfig c; c.enabled = true; c.mode = DyldCacheMode::AllowList; c.store = kStore; c.served = {kLibz}; return c; }
};

class DyldCacheConfigTest : public QObject {
    Q_OBJECT
private slots:
    void missingFileIsTheOffDefault() {
        Fx fx; DyldCacheConfig c; QString err; c.enabled = true;
        QVERIFY2(load(fx.prefix.path(), &c, &err), qPrintable(err));
        QVERIFY(!c.enabled); QCOMPARE(c.mode, DyldCacheMode::Off); QVERIFY(c.served.isEmpty()); QVERIFY(!wantsCache(c));
    }
    void saveWritesConfigAndTheExactPolicy() {
        Fx fx; QString err; QVERIFY2(fx.save(Fx::on(), &err), qPrintable(err));
        DyldCacheConfig back; QVERIFY2(load(fx.prefix.path(), &back, &err), qPrintable(err));
        QCOMPARE(back.store, kStore); QCOMPARE(back.served, QStringList({kLibz})); QVERIFY(back.enabled); QCOMPARE(back.mode, DyldCacheMode::AllowList); QVERIFY(wantsCache(back));
        QCOMPARE(slurp(policyPath(fx.prefix.path())), QByteArray("darling-dyld-cache-policy 1\n/usr/lib/libz.1.dylib\n"));
        QCOMPARE(modeOf(policyPath(fx.prefix.path())), 0644); QCOMPARE(modeOf(configPath(fx.prefix.path())), 0644);
        QCOMPARE(QByteArray(kGuestPolicyPath), QByteArray("/.darling-launcher/dyld-cache.policy"));
    }
    void policyIsRemovedWhenThePrefixDoesNotWantTheCache() {
        for (int variant = 0; variant < 3; ++variant) {
            Fx fx; QVERIFY(fx.save(Fx::on())); QVERIFY(QFileInfo::exists(policyPath(fx.prefix.path())));
            DyldCacheConfig c = Fx::on(); if (variant == 0) c.enabled = false; else if (variant == 1) c.mode = DyldCacheMode::Off; else c.served.clear();
            QString err; QVERIFY2(fx.save(c, &err), qPrintable(err));
            QVERIFY2(!QFileInfo::exists(policyPath(fx.prefix.path())), qPrintable(QString::number(variant)));
            DyldCacheConfig back; QVERIFY(load(fx.prefix.path(), &back, &err)); QCOMPARE(back.served, c.served); // the selection is kept
        }
    }
    void onlyValidatedLibrariesAreSelectable() {
        QCOMPARE(validatedLibraries(), QStringList({kLibz}));
        Fx fx; QString err; DyldCacheConfig c = Fx::on();
        c.served = {"/usr/lib/libfoo.dylib"}; QVERIFY(!fx.save(c, &err)); QVERIFY(err.contains("not validated"));
        c.served = {"@rpath/libz.1.dylib"}; QVERIFY(!fx.save(c, &err));
        c.served = {"/usr/lib/../../etc/passwd"}; QVERIFY(!fx.save(c, &err));
        c.served = {kLibz, kLibz}; QVERIFY(!fx.save(c, &err)); QVERIFY(err.contains("Duplicate"));
        c.served = QStringList(); for (int i = 0; i < 65; ++i) c.served << kLibz; QVERIFY(!fx.save(c, &err)); QVERIFY(err.contains("Too many"));
        c = Fx::on(); c.store = "../x"; QVERIFY(!fx.save(c, &err));
        c = Fx::on(); c.store.clear(); QVERIFY(!fx.save(c, &err));
        QVERIFY(!QFileInfo::exists(configPath(fx.prefix.path()))); QVERIFY(!QFileInfo::exists(policyPath(fx.prefix.path())));
    }
    void loadFailsClosedOnGuestWrittenGarbage() {
        const QList<QByteArray> bad = {
            "not json", "[]", R"({"version":2,"enabled":true,"mode":"off","store":"","served":[]})",
            R"({"version":1,"enabled":"yes","mode":"off","store":"","served":[]})",
            R"({"version":1,"enabled":true,"mode":"cache-first","store":"","served":[]})",
            R"({"version":1,"enabled":true,"mode":"off","store":"","served":[1]})",
            R"({"version":1,"enabled":true,"mode":"off","store":"","served":["/usr/lib/libfoo.dylib"]})",
            R"({"version":1,"enabled":true,"mode":"off","served":[]})",
        };
        for (const QByteArray &b : bad) {
            Fx fx; put(configPath(fx.prefix.path()), b); DyldCacheConfig c; QString err;
            QVERIFY2(!load(fx.prefix.path(), &c, &err), b.constData());
        }
        { Fx fx; put(configPath(fx.prefix.path()), QByteArray(2 << 20, ' ')); DyldCacheConfig c; QString err; QVERIFY(!load(fx.prefix.path(), &c, &err)); }
    }
    void symlinksInTheGuestWritableTreeAreRefused() {
        { Fx fx; put(fx.evil.path() + "/real", "{}"); QDir().mkpath(fx.prefix.path() + "/.darling-launcher");
          QVERIFY(QFile::link(fx.evil.path() + "/real", configPath(fx.prefix.path())));
          DyldCacheConfig c; QString err; QVERIFY(!load(fx.prefix.path(), &c, &err)); }
        { Fx fx; QVERIFY(QFile::link(fx.evil.path(), fx.prefix.path() + "/.darling-launcher"));
          QVERIFY(!fx.save(Fx::on())); QVERIFY(QDir(fx.evil.path()).isEmpty()); }
    }
    void refusesWhileThePrefixMayBeRunning() {
        Fx fx; fx.procFs.socketMayBeLive = [](const QString &) { return true; };
        QString err; QVERIFY(!fx.save(Fx::on(), &err)); QVERIFY(err.contains("running"));
        QVERIFY(!QFileInfo::exists(configPath(fx.prefix.path()))); QVERIFY(!QFileInfo::exists(policyPath(fx.prefix.path())));
    }
    void policyIsWrittenBeforeTheConfig() {
        Fx fx; QDir().mkpath(configPath(fx.prefix.path()) + "/blocker"); // a non-empty directory: replacing it with the file fails
        QString err; QVERIFY(!fx.save(Fx::on(), &err));
        QVERIFY(QFileInfo::exists(policyPath(fx.prefix.path())));
    }
};
QTEST_APPLESS_MAIN(DyldCacheConfigTest)
#include "dyldcacheconfig_test.moc"
