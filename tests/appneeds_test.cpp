// SPDX-License-Identifier: GPL-3.0-or-later
// Every file here is synthetic: Mach-O stubs are built by macho_fixture.h from names this file invents; no Apple files.
#include "appneeds.h"
#include "macho_fixture.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace AppNeeds;

static bool put(const QString &path, const QByteArray &data) {
    return QDir().mkpath(QFileInfo(path).absolutePath()) && [&] { QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(data) == data.size(); }();
}
static QStringList tree(const QString &root) {
    QStringList all;
    QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) { it.next(); all << QDir(root).relativeFilePath(it.filePath()); }
    all.sort();
    return all;
}
static QString fw(const QString &name, bool priv = true) { return QString("/System/Library/%1/%2.framework/Versions/A/%2").arg(priv ? "PrivateFrameworks" : "Frameworks", name); }
static const QString kApp = "System/Applications/Fake.app";
static const QByteArray kMachO64("\xcf\xfa\xed\xfe\x0c\x00\x00\x01 stub", 12);

struct Sandbox {
    QTemporaryDir dir;
    QString volume, prefix;
    Sandbox() {
        volume = dir.path() + "/volume"; prefix = dir.path() + "/prefix";
        QDir().mkpath(volume); QDir().mkpath(prefix);
    }
    void framework(const QString &name, bool priv, const QMap<QString, QByteArray> &resources, bool withInfo = true) {
        const QString base = volume + "/System/Library/" + (priv ? "PrivateFrameworks/" : "Frameworks/") + name + ".framework/Versions/A";
        QVERIFY(put(base + "/Headers/h.h", "// header"));
        if (withInfo) QVERIFY(put(base + "/Resources/Info.plist", "<plist/>"));
        for (auto it = resources.cbegin(); it != resources.cend(); ++it) QVERIFY(put(base + "/Resources/" + it.key(), it.value()));
        QVERIFY(QFile::link("A", volume + "/System/Library/" + (priv ? "PrivateFrameworks/" : "Frameworks/") + name + ".framework/Versions/Current"));
    }
    void app(const QStringList &linked, bool symlinkExecutable = false) {
        const QString bundle = volume + "/" + kApp;
        QVERIFY(put(bundle + "/Contents/Info.plist",
                    "<?xml version=\"1.0\" encoding=\"UTF-8\"?><plist version=\"1.0\"><dict><key>CFBundleExecutable</key><string>Fake</string></dict></plist>"));
        if (symlinkExecutable) {
            QVERIFY(put(dir.path() + "/outside-exe", thin(2, linked)));
            QVERIFY(QDir().mkpath(bundle + "/Contents/MacOS")); QVERIFY(QFile::link(dir.path() + "/outside-exe", bundle + "/Contents/MacOS/Fake"));
        } else QVERIFY(put(bundle + "/Contents/MacOS/Fake", thin(2, linked)));
    }
};

class AppNeedsTest : public QObject {
    Q_OBJECT
    // The standard scene: a family pair, a shared framework, and one framework for each skip reason.
    void scene(Sandbox &s) {
        s.framework("FakeShared", true, {{"model.mom", "model"}, {"code-in-resources", kMachO64}});
        s.framework("FakeUI", true, {{"en.lproj/Main.nib", "nib"}});
        s.framework("Shared", true, {{"big.dat", QByteArray(9000, 's')}});
        s.framework("CodeOnly", true, {{"tool", kMachO64}}, false);
        s.framework("Bare", true, {}, false);
        s.framework("AppKit", false, {{"a.dat", "appkit resource"}});
        s.framework("Present", true, {{"p.dat", "present"}});
        s.framework("Both", true, {{"b.dat", "b"}}); s.framework("Both", false, {{"b.dat", "b"}});
        QVERIFY(put(s.volume + "/System/Library/PrivateFrameworks/Present.framework/Versions/A/Resources/Info.plist", "<plist/>"));
        // the prefix already has Present's files, identical
        const QString pp = s.prefix + "/System/Library/PrivateFrameworks/Present.framework";
        QVERIFY(put(pp + "/Versions/A/Resources/Info.plist", "<plist/>")); QVERIFY(put(pp + "/Versions/A/Resources/p.dat", "present"));
        QVERIFY(QFile::link("A", pp + "/Versions/Current"));
        s.app({fw("FakeShared"), fw("FakeUI"), fw("Shared"), fw("CodeOnly"), fw("Bare"), fw("AppKit", false), fw("Present"), fw("Both"), fw("Missing"),
               "/usr/lib/libSystem.B.dylib", "/usr/lib/swift/libswiftCore.dylib"});
    }
    static const Item *find(const Plan &plan, const QString &name) { for (const Item &i : plan.items) if (i.name == name) return &i; return nullptr; }

private slots:
    void namesComeFromTheExecutableAndEmbeddedLibrariesOnly() {
        Sandbox s;
        s.app({"@rpath/Emb.framework/Versions/A/Emb", "@loader_path/odd.dylib", "@rpath/Gone.framework/Versions/A/Gone", fw("Direct")});
        QVERIFY(put(s.volume + "/" + kApp + "/Contents/Frameworks/Emb.framework/Versions/A/Emb", thin(2, {fw("ViaEmb"), "/usr/lib/libz.1.dylib"})));
        QVERIFY(put(s.volume + "/System/Library/PrivateFrameworks/Direct.framework/Versions/A/Direct", thin(2, {fw("NeverRead")}))); // an Apple image: must not be opened
        QStringList opened;
        Options o; const NameReader inner = windowedReader();
        o.reader = [&](const QString &v, const QString &rel, QList<MachO::Dependency> *n, QString *e) { opened << rel; return inner(v, rel, n, e); };
        Names names; QString error;
        QVERIFY2(linkedNames(s.volume, kApp, &names, &error, o.reader), qPrintable(error));
        QCOMPARE(names.executable, QString("Fake")); QCOMPARE(names.appName, QString("Fake"));
        QStringList got; for (const Linked &l : names.linked) got << l.installName + (l.via.isEmpty() ? "" : " via " + l.via);
        QCOMPARE(got, (QStringList{fw("Direct"), fw("ViaEmb") + " via " + kApp + "/Contents/Frameworks/Emb.framework/Versions/A/Emb", "/usr/lib/libz.1.dylib via " + kApp + "/Contents/Frameworks/Emb.framework/Versions/A/Emb"}));
        QCOMPARE(names.unresolvedRelative, (QStringList{"@loader_path/odd.dylib", "@rpath/Gone.framework/Versions/A/Gone"}));
        QCOMPARE(opened, (QStringList{kApp + "/Contents/MacOS/Fake", kApp + "/Contents/Frameworks/Emb.framework/Versions/A/Emb"})); // nothing under System/
    }
    void embeddedFollowingIsCappedAndConfined() {
        Sandbox s; QStringList many;
        for (int i = 0; i < 70; ++i) many << QString("@rpath/E%1.framework/E%1").arg(i);
        s.app(many);
        for (int i = 0; i < 70; ++i) QVERIFY(put(s.volume + "/" + kApp + QString("/Contents/Frameworks/E%1.framework/E%1").arg(i), thin(2, {})));
        Names names; QString error;
        QVERIFY(!linkedNames(s.volume, kApp, &names, &error)); QVERIFY2(error.contains("embeds more than"), qPrintable(error));
        QVERIFY(!linkedNames(s.volume, "System/Applications/../Fake.app", &names, &error));
        QVERIFY(!linkedNames(s.volume, "System/Applications/Fake", &names, &error));
        Sandbox t; t.app({fw("X")}, true); // the executable is a symlink out of the volume
        error.clear(); QVERIFY(!linkedNames(t.volume, kApp, &names, &error)); QVERIFY(!error.isEmpty());
    }
    void everyRowHasItsReason() {
        Sandbox s; scene(s);
        Plan plan; QString error;
        QVERIFY2(buildPlan(s.volume, s.prefix, kApp, {}, &plan, &error), qPrintable(error));
        QCOMPARE(find(plan, "FakeShared")->status, Status::Selected);
        QCOMPARE(find(plan, "FakeUI")->status, Status::Selected);
        QCOMPARE(find(plan, "Shared")->status, Status::Optional);
        QCOMPARE(find(plan, "AppKit")->reason, QString("provided by Darling"));
        QCOMPARE(find(plan, "Missing")->reason, QString("not on the macOS volume"));
        QCOMPARE(find(plan, "Present")->reason, QString("resources already present and identical"));
        QCOMPARE(find(plan, "CodeOnly")->reason, QString("no resource files to copy")); // only code in Resources
        QCOMPARE(find(plan, "Bare")->reason, QString("no resource files to copy"));
        QVERIFY(find(plan, "Both")->reason.contains("both Frameworks and PrivateFrameworks"));
        QCOMPARE(plan.notFrameworkBundles, (QStringList{"/usr/lib/libSystem.B.dylib", "/usr/lib/swift/libswiftCore.dylib"}));
        QCOMPARE(plan.selectedNames(), (QStringList{"FakeShared", "FakeUI"}));
        QVERIFY(plan.selectedFiles() >= 3);
    }
    void alsoPromotesOptionalOnlyAndRefusesTheRest() {
        Sandbox s; scene(s);
        Plan plan; QString error; Options o; o.also = {"Shared"};
        QVERIFY(buildPlan(s.volume, s.prefix, kApp, o, &plan, &error));
        QCOMPARE(find(plan, "Shared")->status, Status::Selected); QCOMPARE(find(plan, "Shared")->reason, QString("chosen by you"));
        o.also = {"AppKit"}; QVERIFY(!buildPlan(s.volume, s.prefix, kApp, o, &plan, &error)); QVERIFY2(error.contains("provided by Darling"), qPrintable(error));
        o.also = {"NotLinked"}; QVERIFY(!buildPlan(s.volume, s.prefix, kApp, o, &plan, &error)); QVERIFY2(error.contains("not a framework this app links"), qPrintable(error));
    }
    void summaryNamesEverythingTheUserDecidesOn() {
        Sandbox s; scene(s);
        Plan plan; QString error; QVERIFY(buildPlan(s.volume, s.prefix, kApp, {}, &plan, &error));
        const QString text = summarize(plan);
        for (const QString &needle : {"Selected framework resources: 2", "FakeShared", "FakeUI", "Optional (linked, not selected): 1", "Shared:", "Skipped: 6",
                                      "AppKit: provided by Darling", "Free space on the prefix", "never executable code"})
            QVERIFY2(text.contains(needle), qPrintable(needle + "\n" + text));
    }
    void nothingIsCopiedWithoutConfirmation() {
        Sandbox s; scene(s);
        Plan plan; QString error; QVERIFY(buildPlan(s.volume, s.prefix, kApp, {}, &plan, &error));
        const QStringList before = tree(s.prefix);
        Applied applied;
        QVERIFY(!applyResources(s.volume, s.prefix, plan, false, {}, &applied, &error)); QVERIFY2(error.contains("Confirmation is required"), qPrintable(error));
        QCOMPARE(tree(s.prefix), before);
    }
    void confirmedApplyCopiesOnlyResourcesOfTheSelectedAndLogsOnce() {
        Sandbox s; scene(s);
        Plan plan; QString error; QVERIFY(buildPlan(s.volume, s.prefix, kApp, {}, &plan, &error));
        Applied applied;
        QVERIFY2(applyResources(s.volume, s.prefix, plan, true, {}, &applied, &error), qPrintable(error));
        QCOMPARE(applied.bundles, (QStringList{"FakeShared", "FakeUI"}));
        const QString p = s.prefix + "/System/Library/PrivateFrameworks/";
        QVERIFY(QFile::exists(p + "FakeShared.framework/Versions/A/Resources/model.mom"));
        QVERIFY(QFile::exists(p + "FakeUI.framework/Versions/A/Resources/en.lproj/Main.nib"));
        QVERIFY(!QFile::exists(p + "FakeShared.framework/Versions/A/Resources/code-in-resources")); // Mach-O never copied
        QVERIFY(!QFile::exists(p + "FakeShared.framework/Versions/A/Headers"));
        QVERIFY(!QFileInfo::exists(p + "Shared.framework")); QVERIFY(!QFileInfo::exists(s.prefix + "/System/Library/Frameworks/AppKit.framework")); // optional and Darling's stay out
        QFile log(logPath(s.prefix)); QVERIFY(log.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(log.readAll());
        QVERIFY(text.contains("| Fake | framework resources | FakeShared, FakeUI |")); QCOMPARE(text.count("framework resources"), 1);
        // idempotent: a second plan finds nothing to select, a repeated apply of the old plan copies nothing and adds no row
        Applied again; QVERIFY2(applyResources(s.volume, s.prefix, plan, true, {}, &again, &error), qPrintable(error));
        QCOMPARE(again.result.files, 0);
        QFile log2(logPath(s.prefix)); QVERIFY(log2.open(QIODevice::ReadOnly)); QCOMPARE(QString::fromUtf8(log2.readAll()).count("framework resources"), 1);
        Plan second; QVERIFY(buildPlan(s.volume, s.prefix, kApp, {}, &second, &error));
        QCOMPARE(find(second, "FakeUI")->reason, QString("resources already present and identical")); QCOMPARE(second.selectedCount(), 0);
    }
    void aShortDiskRefusesBeforeAnythingIsWritten() {
        Sandbox s; scene(s);
        Options o; o.freeBytes = [](const QString &) { return qint64(1) << 20; }; // 1 MiB free: below the 512 MiB reserve
        Plan plan; QString error; QVERIFY(buildPlan(s.volume, s.prefix, kApp, o, &plan, &error));
        const QStringList before = tree(s.prefix); Applied applied;
        QVERIFY(!applyResources(s.volume, s.prefix, plan, true, o, &applied, &error)); QVERIFY2(error.contains("Not enough free space"), qPrintable(error));
        QCOMPARE(tree(s.prefix), before);
        o.freeBytes = [](const QString &) { return qint64(-1); };
        QVERIFY(!applyResources(s.volume, s.prefix, plan, true, o, &applied, &error)); QVERIFY2(error.contains("unknown"), qPrintable(error));
    }
    void aSymlinkThatLeavesAFrameworkIsNeverCopied() {
        Sandbox s; scene(s);
        QVERIFY(put(s.dir.path() + "/secret", "private"));
        QVERIFY(QFile::link("../../../../../../../../../secret", s.volume + "/System/Library/PrivateFrameworks/FakeUI.framework/Versions/A/Resources/escape"));
        Plan plan; QString error; QVERIFY(buildPlan(s.volume, s.prefix, kApp, {}, &plan, &error));
        Applied applied; QVERIFY2(applyResources(s.volume, s.prefix, plan, true, {}, &applied, &error), qPrintable(error));
        QVERIFY(!QFileInfo(s.prefix + "/System/Library/PrivateFrameworks/FakeUI.framework/Versions/A/Resources/escape").isSymLink());
        QVERIFY(!QFileInfo::exists(s.prefix + "/System/Library/PrivateFrameworks/FakeUI.framework/Versions/A/Resources/escape"));
    }
    void anUnreadableEmbeddedImageIsReportedNotFatal() {
        Sandbox s;
        s.app({"@rpath/Emb.framework/Versions/Current/Emb", "@rpath/Script.framework/Script", fw("Direct")});
        QVERIFY(put(s.volume + "/" + kApp + "/Contents/Frameworks/Emb.framework/Versions/A/Emb", thin(2, {fw("ViaEmb")})));
        QVERIFY(QFile::link("A", s.volume + "/" + kApp + "/Contents/Frameworks/Emb.framework/Versions/Current"));       // behind a link: the safe reader refuses it
        QVERIFY(put(s.volume + "/" + kApp + "/Contents/Frameworks/Script.framework/Script", "#!/bin/sh\n"));              // not a Mach-O
        Names names; QString error;
        QVERIFY2(linkedNames(s.volume, kApp, &names, &error), qPrintable(error));
        QCOMPARE(names.linked.size(), 1); QCOMPARE(names.linked[0].installName, fw("Direct"));
        QCOMPARE(names.unresolvedRelative.size(), 2); QVERIFY(names.unresolvedRelative[0].contains("not readable"));
    }
    void aTraversingRpathTailAndALinkedInfoPlistAreRefused() {
        Sandbox s; s.app({"@rpath/../../outside.dylib"});
        QVERIFY(put(s.dir.path() + "/outside.dylib", thin(2, {fw("Escaped")})));
        QVERIFY(put(s.volume + "/outside.dylib", thin(2, {fw("Escaped")})));
        Names names; QString error;
        QVERIFY(linkedNames(s.volume, kApp, &names, &error)); QVERIFY(names.linked.isEmpty()); QCOMPARE(names.unresolvedRelative, QStringList{"@rpath/../../outside.dylib"});
        Sandbox t; t.app({fw("X")});
        const QString plist = t.volume + "/" + kApp + "/Contents/Info.plist";
        QVERIFY(QFile::rename(plist, t.dir.path() + "/real.plist")); QVERIFY(QFile::link(t.dir.path() + "/real.plist", plist));
        QVERIFY(!linkedNames(t.volume, kApp, &names, &error)); QVERIFY2(error.contains("Info.plist"), qPrintable(error));
    }
    void aShortAppNameDoesNotPullInUnrelatedFrameworks() {
        Sandbox s;
        s.framework("CoreThing", true, {{"a.dat", "a"}});
        QVERIFY(put(s.volume + "/System/Applications/Cor.app/Contents/Info.plist", "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>CFBundleExecutable</key><string>Cor</string></dict></plist>"));
        QVERIFY(put(s.volume + "/System/Applications/Cor.app/Contents/MacOS/Cor", thin(2, {fw("CoreThing")})));
        Plan plan; QString error; QVERIFY2(buildPlan(s.volume, s.prefix, "System/Applications/Cor.app", {}, &plan, &error), qPrintable(error));
        QCOMPARE(find(plan, "CoreThing")->status, Status::Optional);
    }
    void aFailedLogWriteAfterTheCopyIsReported() {
        Sandbox s; scene(s);
        Plan plan; QString error; QVERIFY(buildPlan(s.volume, s.prefix, kApp, {}, &plan, &error));
        QVERIFY(put(s.prefix + "/.darling-launcher/app-imports.md", QByteArray(1200000, 'x'))); // above the log's read cap
        Applied applied; QVERIFY(!applyResources(s.volume, s.prefix, plan, true, {}, &applied, &error));
        QVERIFY2(error.contains("copied, but the provenance log"), qPrintable(error));
    }
    void logFieldsCannotBreakTheTable() {
        Sandbox s; QVERIFY(QDir().mkpath(s.prefix + "/.darling-launcher")); QString error;
        QVERIFY(appendLog(s.prefix, "Bad|App\nName", "step", "a|b", "1", "x\ny", &error));
        QFile log(logPath(s.prefix)); QVERIFY(log.open(QIODevice::ReadOnly));
        const QStringList lines = QString::fromUtf8(log.readAll()).split('\n', Qt::SkipEmptyParts);
        QCOMPARE(lines.last().count('|'), 7); // a row of six cells
        QVERIFY(!lines.last().contains("Bad|App"));
    }
};
QTEST_APPLESS_MAIN(AppNeedsTest)
#include "appneeds_test.moc"
