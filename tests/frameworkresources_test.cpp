// SPDX-License-Identifier: GPL-3.0-or-later
// Every file here is synthetic: Mach-O stubs are a real magic number followed by filler, never Apple bytes.
#include "frameworkresources.h"
#include "frameworkscli.h"
#include "macho.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace FrameworkResources;

static const QByteArray kMachO64("\xcf\xfa\xed\xfe\x0c\x00\x00\x01 stub", 12);
static const QByteArray kFat("\xca\xfe\xba\xbe\x00\x00\x00\x01 stub", 12);

static bool put(const QString &path, const QByteArray &data) {
    return QDir().mkpath(QFileInfo(path).absolutePath()) && [&] { QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(data) == data.size(); }();
}
static QByteArray slurp(const QString &path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }
static bool link(const QString &target, const QString &path) { return QDir().mkpath(QFileInfo(path).absolutePath()) && QFile::link(target, path); }
static QStringList relativeTree(const QString &root) {
    QStringList all;
    QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) { it.next(); all << QDir(root).relativeFilePath(it.filePath()); }
    all.sort();
    return all;
}

class FrameworkResourcesTest : public QObject {
    Q_OBJECT
    static constexpr const char *Fake = "System/Library/Frameworks/Fake.framework";
    static constexpr const char *Priv = "System/Library/PrivateFrameworks/Priv.framework";

    // A versioned framework with resources, code of every kind, and links; a flat private framework.
    void buildVolume(const QString &volume, const QString &outside) {
        const QString a = volume + "/" + Fake + "/Versions/A", res = a + "/Resources";
        QVERIFY(put(a + "/Fake", kMachO64));                      // the framework binary
        QVERIFY(put(res + "/Info.plist", "<plist>fake</plist>"));
        QVERIFY(put(res + "/version.plist", "<plist>version</plist>"));
        QVERIFY(put(res + "/en.lproj/Localizable.strings", "\"a\" = \"b\";"));
        QVERIFY(put(res + "/Emoji/emoji.dat", QByteArray(5000, 'e')));
        QVERIFY(put(res + "/empty.dat", ""));
        QVERIFY(put(res + "/helper-tool", kMachO64));             // Mach-O without a telling name
        QVERIFY(put(res + "/fat-tool", kFat));
        QVERIFY(put(res + "/renamed.png", kMachO64));             // Mach-O hiding behind a data name
        QVERIFY(put(res + "/Plugins/p.txt", "x"));                                  // a code directory under another case
        QVERIFY(put(res + "/script.sh", "#!/bin/sh\necho hi\n"));
        QVERIFY(put(res + "/shaders.dat", "MTLB\x01\x00 compiled gpu code"));       // metallib by magic under a data name
        QVERIFY(put(res + "/default.metallib", "plain bytes"));                      // and by name
        QVERIFY(put(res + "/tool.scpt", "FasdUAS 1.101.10 compiled applescript"));   // compiled AppleScript
        QVERIFY(put(res + "/renamed.txt", "FasdUAS 1.101.10 compiled applescript")); // by magic under a data name
        QVERIFY(put(res + "/libplain.dylib", "not really code"));  // skipped by name as well
        QVERIFY(put(res + "/XPCServices/Svc.xpc/Info.plist", "x"));
        QVERIFY(put(res + "/PlugIns/p/data", "x"));
        QVERIFY(put(res + "/Tool.app/Contents/Info.plist", "x"));
        QVERIFY(put(res + "/Plug.bundle/Contents/MacOS/exe", kMachO64));
        QVERIFY(put(res + "/Plug.bundle/Contents/Resources/r.txt", "bundle resource"));
        QVERIFY(put(a + "/XPCServices/other.xpc/x", "x"));
        QVERIFY(put(a + "/PlugIns/y", "x"));
        QVERIFY(put(a + "/Helpers/h", "x"));
        QVERIFY(put(a + "/Headers/Fake.h", "// header"));
        QVERIFY(put(outside + "/secret", "private"));
        QVERIFY(link("data-target", res + "/dangling"));
        QVERIFY(link("Emoji/emoji.dat", res + "/emoji-link"));      // a link to data: kept
        QVERIFY(link("helper-tool", res + "/tool-link"));           // a link to code: dropped
        QVERIFY(link("shaders.dat", res + "/shaders-link"));        // to compiled GPU code
        QVERIFY(link("renamed.txt", res + "/script-link"));         // to compiled AppleScript
        QVERIFY(link("../../../../../../../../outside/secret", res + "/escape"));
        QVERIFY(link("/etc/hostname", res + "/absolute"));
        QVERIFY(link("A", volume + "/" + Fake + "/Versions/Current"));
        QVERIFY(link("Versions/Current/Resources", volume + "/" + Fake + "/Resources"));
        QVERIFY(link("Versions/Current/Fake", volume + "/" + Fake + "/Fake"));
        QVERIFY(put(a + "/Frameworks/Inner.framework/Info.plist", "inner"));
        QVERIFY(put(a + "/Frameworks/Inner.framework/Resources/inner.txt", "inner resource"));
        QVERIFY(put(a + "/Frameworks/Inner.framework/Inner", kMachO64));
        QVERIFY(put(volume + "/" + Priv + "/Info.plist", "priv"));
        QVERIFY(put(volume + "/" + Priv + "/Resources/p.txt", "private resource"));
        QVERIFY(put(volume + "/" + Priv + "/Priv", kMachO64));
        QVERIFY(put(volume + "/Applications/Unrelated.app/Contents/Info.plist", "x"));
    }
    struct Setup { QString volume, prefix, outside; };
    Setup layout(const QTemporaryDir &t) { return {t.path() + "/volume", t.path() + "/prefix", t.path() + "/outside"}; }

private slots:
    void dryRunCopiesNothing() {
        QTemporaryDir t; QVERIFY(t.isValid()); const auto s = layout(t);
        buildVolume(s.volume, s.outside); QVERIFY(QDir().mkpath(s.prefix));
        const QStringList before = relativeTree(s.volume);
        Plan plan; QString error;
        QVERIFY2(buildPlan(s.volume, s.prefix, &plan, &error), qPrintable(error));
        QCOMPARE(plan.frameworks, 3);
        QVERIFY(plan.files > 0 && plan.bytes > 5000);
        QVERIFY(QDir(s.prefix).isEmpty());                // nothing was created
        QCOMPARE(relativeTree(s.volume), before);         // and the source is untouched
        QVERIFY(summarize(plan).contains("Mach-O files"));
    }
    void importsResourcesAndSkipsCode() {
        QTemporaryDir t; const auto s = layout(t);
        buildVolume(s.volume, s.outside); QVERIFY(QDir().mkpath(s.prefix));
        Plan plan; Result result; QString error;
        QVERIFY2(buildPlan(s.volume, s.prefix, &plan, &error), qPrintable(error));
        // three framework binaries (Fake, Inner, Priv), helper-tool, fat-tool, renamed.png and the link to helper-tool;
        // Plug.bundle/Contents/MacOS/exe sits in a code directory and is counted by name instead
        QCOMPARE(plan.skippedMachO, 7);
        QCOMPARE(plan.skippedScripts, 5);                                  // script.sh, shaders.dat, renamed.txt and the two links to code
        QVERIFY(plan.skippedByName >= 2);                                  // default.metallib, tool.scpt (and the code directories)
        QVERIFY2(applyPlan(s.volume, s.prefix, plan, &result, &error), qPrintable(error));
        QCOMPARE(result.files, plan.files); QCOMPARE(result.bytes, plan.apparentBytes); QVERIFY(plan.bytes >= plan.apparentBytes + 4096 * quint64(plan.directories + plan.symlinks));
        QVERIFY(plan.bytes >= 4096 * quint64(plan.files + plan.directories + plan.symlinks)); // a block each, even for the empty file
        Plan unknown; unknown.availableBytes = -1; QVERIFY(!unknown.enoughSpace());          // unknown free space is not room
        unknown.availableBytes = 1ll << 40; QVERIFY(unknown.enoughSpace());
        const QString fake = s.prefix + "/" + Fake, res = fake + "/Versions/A/Resources";
        QCOMPARE(slurp(res + "/Info.plist"), QByteArray("<plist>fake</plist>"));
        QCOMPARE(slurp(res + "/version.plist"), QByteArray("<plist>version</plist>"));
        QCOMPARE(slurp(res + "/en.lproj/Localizable.strings"), QByteArray("\"a\" = \"b\";"));
        QCOMPARE(slurp(res + "/Emoji/emoji.dat"), QByteArray(5000, 'e'));
        QVERIFY(QFileInfo(res + "/empty.dat").isFile());
        QCOMPARE(slurp(res + "/Plug.bundle/Contents/Resources/r.txt"), QByteArray("bundle resource"));
        QCOMPARE(slurp(fake + "/Versions/A/Frameworks/Inner.framework/Resources/inner.txt"), QByteArray("inner resource"));
        QCOMPARE(slurp(s.prefix + "/" + Priv + "/Resources/p.txt"), QByteArray("private resource"));
        QCOMPARE(slurp(s.prefix + "/" + Priv + "/Info.plist"), QByteArray("priv"));
        // layout: links are links, with the same text, and resolve inside the prefix
        QVERIFY(QFileInfo(fake + "/Versions/Current").isSymLink());
        QCOMPARE(QFile::symLinkTarget(fake + "/Versions/Current"), fake + "/Versions/A");
        QVERIFY(QFileInfo(fake + "/Resources").isSymLink());
        QCOMPARE(QFileInfo(fake + "/Resources/Info.plist").canonicalFilePath(), QFileInfo(res + "/Info.plist").canonicalFilePath());
        QVERIFY(QFileInfo(res + "/emoji-link").isSymLink());
        QCOMPARE(slurp(res + "/emoji-link"), QByteArray(5000, 'e'));
        // nothing that is code, nothing that was refused
        for (const char *gone : {"/Versions/A/Fake", "/Fake", "/Versions/A/XPCServices", "/Versions/A/PlugIns", "/Versions/A/Helpers", "/Versions/A/Headers",
                                 "/Versions/A/Resources/helper-tool", "/Versions/A/Resources/fat-tool", "/Versions/A/Resources/renamed.png",
                                 "/Versions/A/Resources/script.sh", "/Versions/A/Resources/libplain.dylib", "/Versions/A/Resources/shaders.dat", "/Versions/A/Resources/default.metallib", "/Versions/A/Resources/tool.scpt", "/Versions/A/Resources/renamed.txt", "/Versions/A/Resources/XPCServices",
                                 "/Versions/A/Resources/PlugIns", "/Versions/A/Resources/Tool.app", "/Versions/A/Resources/Plug.bundle/Contents/MacOS",
                                 "/Versions/A/Resources/tool-link", "/Versions/A/Resources/shaders-link", "/Versions/A/Resources/script-link", "/Versions/A/Resources/Plugins", "/Versions/A/Resources/escape", "/Versions/A/Resources/absolute",
                                 "/Versions/A/Resources/dangling", "/Versions/A/Frameworks/Inner.framework/Inner"})
            QVERIFY2(!QFileInfo(fake + gone).isSymLink() && !QFileInfo::exists(fake + gone), gone);
        QVERIFY(!QFileInfo::exists(s.prefix + "/" + Priv + "/Priv"));
        QVERIFY(!QFileInfo::exists(s.prefix + "/Applications"));
        QCOMPARE(plan.refused.size(), 3);                 // escape, absolute, dangling
        // the tree holds no Mach-O, no script, no *.dylib, wherever it is
        QDirIterator it(s.prefix, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QByteArray bytes = slurp(it.next());
            QVERIFY2(!MachO::hasMachOMagic(bytes.left(4)) && !bytes.startsWith("#!") && !bytes.startsWith("MTLB") && !bytes.startsWith("FasdUAS") && !it.filePath().endsWith(".dylib") && !it.filePath().endsWith(".metallib") && !it.filePath().endsWith(".scpt"), qPrintable(it.filePath()));
        }
        QCOMPARE(slurp(s.outside + "/secret"), QByteArray("private"));
    }
    void existingDarlingFilesAreNeverReplaced() {
        QTemporaryDir t; const auto s = layout(t);
        buildVolume(s.volume, s.outside);
        const QString fake = s.prefix + "/" + Fake, res = fake + "/Versions/A/Resources";
        QVERIFY(put(fake + "/Versions/A/Fake", "darling-built binary"));            // Darling's own binary
        QVERIFY(put(res + "/Info.plist", "darling info"));                          // differs: a conflict
        QVERIFY(put(res + "/version.plist", "<plist>version</plist>"));             // identical: already present
        QVERIFY(link("Versions/Current/Elsewhere", fake + "/Resources"));           // a link with another target: a conflict
        QVERIFY(put(s.prefix + "/" + Priv + "/Info.plist", "priv"));
        QVERIFY(link("/nonexistent", s.prefix + "/" + Priv + "/Resources"));        // a symlink where a directory would go
        Plan plan; Result result; QString error;
        QVERIFY2(buildPlan(s.volume, s.prefix, &plan, &error), qPrintable(error));
        QVERIFY(plan.conflicts.contains(QString(Fake) + "/Versions/A/Resources/Info.plist"));
        QVERIFY(plan.conflicts.contains(QString(Fake) + "/Resources"));
        QVERIFY(plan.conflicts.contains(QString(Priv) + "/Resources"));
        QVERIFY(plan.alreadyPresent >= 2);                                          // version.plist and the Priv Info.plist
        QVERIFY2(applyPlan(s.volume, s.prefix, plan, &result, &error), qPrintable(error));
        QCOMPARE(slurp(fake + "/Versions/A/Fake"), QByteArray("darling-built binary"));
        QCOMPARE(slurp(res + "/Info.plist"), QByteArray("darling info"));
        QCOMPARE(QFile::symLinkTarget(fake + "/Resources"), QDir::cleanPath(fake + "/Versions/Current/Elsewhere"));
        QCOMPARE(QFile::symLinkTarget(s.prefix + "/" + Priv + "/Resources"), QString("/nonexistent"));
        QVERIFY(!QFileInfo::exists(s.prefix + "/" + Priv + "/Resources/p.txt"));
        QCOMPARE(slurp(res + "/Emoji/emoji.dat"), QByteArray(5000, 'e'));           // the missing files were added
        QVERIFY(summarize(plan).contains("Conflicts, left unchanged: 3"));
    }
    void rerunIsIdempotentAndResumes() {
        QTemporaryDir t; const auto s = layout(t);
        buildVolume(s.volume, s.outside); QVERIFY(QDir().mkpath(s.prefix));
        Plan plan; Result result; QString error;
        QVERIFY2(buildPlan(s.volume, s.prefix, &plan, &error), qPrintable(error));
        // a cut-short earlier run: part of the work is done, one file is a stray partial copy
        const QString res = s.prefix + "/" + Fake + "/Versions/A/Resources";
        const QString part = res + "/Emoji/.dlpart-" + QString::fromLatin1(QCryptographicHash::hash("emoji.dat", QCryptographicHash::Sha1).toHex().left(16));
        QVERIFY(put(part, "partial"));
        QCOMPARE(plan.entries.size() > 0, true);
        int stop = plan.entries.size() / 2, seen = 0;
        QVERIFY(!applyPlan(s.volume, s.prefix, plan, &result, &error, {}, [&] { return seen++ >= stop; }));
        QVERIFY2(error.contains("Cancelled") && error.contains("resume"), qPrintable(error));
        QVERIFY(result.files > 0);
        QVERIFY2(buildPlan(s.volume, s.prefix, &plan, &error), qPrintable(error));
        QVERIFY2(applyPlan(s.volume, s.prefix, plan, &result, &error), qPrintable(error));
        QVERIFY(!QFileInfo::exists(part));
        QCOMPARE(slurp(res + "/Emoji/emoji.dat"), QByteArray(5000, 'e'));
        const QStringList done = relativeTree(s.prefix);
        Plan again; Result none;
        QVERIFY2(buildPlan(s.volume, s.prefix, &again, &error), qPrintable(error));
        QVERIFY(again.entries.isEmpty()); QCOMPARE(again.bytes, quint64(0)); QVERIFY(again.conflicts.isEmpty()); QVERIFY(again.alreadyPresent > 0);
        QVERIFY2(applyPlan(s.volume, s.prefix, again, &none, &error), qPrintable(error));
        QCOMPARE(none.files + none.directories + none.symlinks, 0);
        QCOMPARE(relativeTree(s.prefix), done);
    }
    void refusesWhenThereIsNoRoomOrTheSourceIsWrong() {
        QTemporaryDir t; const auto s = layout(t);
        buildVolume(s.volume, s.outside); QVERIFY(QDir().mkpath(s.prefix));
        Plan plan; Result result; QString error;
        QVERIFY2(buildPlan(s.volume, s.prefix, &plan, &error), qPrintable(error));
        plan.bytes = quint64(1) << 60;
        QVERIFY(!plan.enoughSpace() || plan.availableBytes < 0);
        QVERIFY(!applyPlan(s.volume, s.prefix, plan, &result, &error)); QVERIFY2(error.contains("Not enough free space"), qPrintable(error));
        QVERIFY(QDir(s.prefix).isEmpty());
        QVERIFY(!buildPlan(s.prefix, s.volume, &plan, &error));                    // no frameworks in what is supposed to be the volume
        QVERIFY(QDir().mkpath(t.path() + "/bare/System/Library")); QVERIFY(QDir().mkpath(t.path() + "/p2"));
        QVERIFY(!buildPlan(t.path() + "/bare", t.path() + "/p2", &plan, &error)); QVERIFY2(error.contains("Nothing to import"), qPrintable(error));
        QVERIFY(QFile::link(s.outside, s.prefix + "/System")); // a symlink in the prefix is never followed
        QVERIFY(!buildPlan(s.volume, s.prefix, &plan, &error)); QVERIFY2(error.contains("symlink"), qPrintable(error));
        QVERIFY(!QFileInfo::exists(s.outside + "/Library"));
    }
    void longNamesAndForeignPartFilesDoNotBlockOrGetDeleted() {
        QTemporaryDir t; const auto s = layout(t);
        buildVolume(s.volume, s.outside); QVERIFY(QDir().mkpath(s.prefix));
        const QString res = QString(Fake) + "/Versions/A/Resources/";
        const QString longName = QString(250, 'a') + ".txt";
        QVERIFY(put(s.volume + "/" + res + longName, "long"));
        QVERIFY(put(s.prefix + "/" + res + "s1.txt.dlpart", "the user's own file"));   // not one of ours
        QVERIFY(put(s.volume + "/" + res + "s1.txt", "s1"));
        Plan plan; Result result; QString error;
        QVERIFY2(buildPlan(s.volume, s.prefix, &plan, &error), qPrintable(error));
        QVERIFY2(applyPlan(s.volume, s.prefix, plan, &result, &error), qPrintable(error));
        QCOMPARE(slurp(s.prefix + "/" + res + longName), QByteArray("long"));
        QCOMPARE(slurp(s.prefix + "/" + res + "s1.txt"), QByteArray("s1"));
        QCOMPARE(slurp(s.prefix + "/" + res + "s1.txt.dlpart"), QByteArray("the user's own file"));
    }
    void codeThatReplacesAPlannedFileIsNotCopied() {
        QTemporaryDir t; const auto s = layout(t);
        buildVolume(s.volume, s.outside); QVERIFY(QDir().mkpath(s.prefix));
        Plan plan; Result result; QString error;
        QVERIFY2(buildPlan(s.volume, s.prefix, &plan, &error), qPrintable(error));
        const QString rel = QString(Fake) + "/Versions/A/Resources/Emoji/emoji.dat";
        QVERIFY(put(s.volume + "/" + rel, kMachO64));                                  // swapped after the confirmation
        QVERIFY(!applyPlan(s.volume, s.prefix, plan, &result, &error));
        QVERIFY2(error.contains("executable code"), qPrintable(error));
        QVERIFY(!QFileInfo::exists(s.prefix + "/" + rel));
        QDirIterator it(s.prefix, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext()) QVERIFY2(!MachO::hasMachOMagic(slurp(it.next()).left(4)), qPrintable(it.filePath()));
    }
    void anEntryThatAppearsAfterThePlanIsLeftAlone() {
        QTemporaryDir t; const auto s = layout(t);
        buildVolume(s.volume, s.outside); QVERIFY(QDir().mkpath(s.prefix));
        Plan plan; Result result; QString error;
        QVERIFY2(buildPlan(s.volume, s.prefix, &plan, &error), qPrintable(error));
        const QString info = s.prefix + "/" + Fake + "/Versions/A/Resources/Info.plist";
        QVERIFY(put(info, "appeared meanwhile"));
        QVERIFY2(applyPlan(s.volume, s.prefix, plan, &result, &error), qPrintable(error));
        QCOMPARE(slurp(info), QByteArray("appeared meanwhile")); QCOMPARE(result.appeared, 1);
    }
    void onlyImportsTheNamedBundleAndItsNestedFrameworks() {
        QTemporaryDir t; const auto s = layout(t);
        buildVolume(s.volume, s.outside); QVERIFY(QDir().mkpath(s.prefix));
        Plan plan; QString error;
        QVERIFY2(buildPlan(s.volume, s.prefix, &plan, &error, {}, {"Priv"}), qPrintable(error));
        QCOMPARE(plan.frameworks, 1);
        for (const Entry &e : plan.entries) QVERIFY2(e.rel.startsWith(QString(Priv)), qPrintable(e.rel));
        QVERIFY(std::any_of(plan.entries.begin(), plan.entries.end(), [](const Entry &e) { return e.rel == QString(Priv) + "/Resources/p.txt"; }));
        Plan fake; // the other bundle brings Inner along, and the code rules still apply
        QVERIFY2(buildPlan(s.volume, s.prefix, &fake, &error, {}, {"Fake"}), qPrintable(error));
        QCOMPARE(fake.frameworks, 2);
        bool inner = false;
        for (const Entry &e : fake.entries) {
            QVERIFY2(e.rel.startsWith(QString(Fake)), qPrintable(e.rel));
            inner = inner || e.rel.endsWith("Inner.framework/Resources/inner.txt");
        }
        QVERIFY(inner); QVERIFY(fake.skippedMachO > 0);
        Plan both; // several names, and the same name twice
        QVERIFY2(buildPlan(s.volume, s.prefix, &both, &error, {}, {"Fake", "Priv", "Priv"}), qPrintable(error));
        Plan all; QVERIFY2(buildPlan(s.volume, s.prefix, &all, &error), qPrintable(error));
        QCOMPARE(both.entries.size(), all.entries.size());
        Result result;
        QVERIFY2(applyPlan(s.volume, s.prefix, plan, &result, &error), qPrintable(error));
        QVERIFY(QFileInfo::exists(s.prefix + "/" + Priv + "/Resources/p.txt"));
        QVERIFY(!QFileInfo::exists(s.prefix + "/" + Fake));
    }
    void onlyWithAnUnknownNameFailsAndListsIt() {
        QTemporaryDir t; const auto s = layout(t);
        buildVolume(s.volume, s.outside); QVERIFY(QDir().mkpath(s.prefix));
        Plan plan; QString error;
        QVERIFY(!buildPlan(s.volume, s.prefix, &plan, &error, {}, {"Priv", "Nope", "Fake.framework"}));
        QVERIFY2(error.contains("Nope") && error.contains("Fake.framework") && !error.contains("Priv,"), qPrintable(error));
        QVERIFY(!buildPlan(s.volume, s.prefix, &plan, &error, {}, {"fake"})); // exact, case-sensitive
        QVERIFY(!buildPlan(s.volume, s.prefix, &plan, &error, {}, {"Inner"})); // nested frameworks are not top-level names
        QVERIFY(QDir(s.prefix).isEmpty());
    }
    void cliOnly() {
        QTemporaryDir t; const auto s = layout(t);
        buildVolume(s.volume, s.outside); QVERIFY(QDir().mkpath(s.prefix));
        auto run = [&](const QStringList &args, QString *errText) {
            QString o, e; int code; { QTextStream out(&o), err(&e); code = frameworksImportMain(args, out, err); }
            *errText = e.trimmed(); return code;
        };
        const QStringList base{"--volume", s.volume, "--prefix", s.prefix};
        QString err;
        QCOMPARE(run(base + QStringList{"--only", "Nope", "--dry-run"}, &err), 1); QVERIFY2(err.contains("Nope"), qPrintable(err));
        QCOMPARE(run(base + QStringList{"--only", "Priv,", "--dry-run"}, &err), 2);
        QCOMPARE(run(base + QStringList{"--only", "Priv", "--only", "Fake", "--dry-run"}, &err), 2);
        QCOMPARE(run(base + QStringList{"--only", "Priv", "--yes"}, &err), 0);
        QVERIFY(QFileInfo::exists(s.prefix + "/" + Priv + "/Resources/p.txt"));
        QVERIFY(!QFileInfo::exists(s.prefix + "/" + Fake));
        QCOMPARE(run(base + QStringList{"--only", "Priv,Nope", "--yes"}, &err), 1);
        QVERIFY(!QFileInfo::exists(s.prefix + "/" + Fake));
    }
    void cliDryRunNeedsYesToCopy() {
        QTemporaryDir t; const auto s = layout(t);
        buildVolume(s.volume, s.outside); QVERIFY(QDir().mkpath(s.prefix));
        auto run = [&](const QStringList &args, QString *outText, QString *errText) {
            QString o, e; int code; { QTextStream out(&o), err(&e); code = frameworksImportMain(args, out, err); }
            *outText = o; *errText = e.trimmed(); return code;
        };
        const QStringList base{"--volume", s.volume, "--prefix", s.prefix};
        QString out, err;
        QCOMPARE(run({"--volume", s.volume}, &out, &err), 2);
        QCOMPARE(run(base + QStringList{"--dry-run", "--yes"}, &out, &err), 2);
        QCOMPARE(run(base + QStringList{"--volume", s.volume}, &out, &err), 2);
        QCOMPARE(run(base + QStringList{"--dry-run"}, &out, &err), 0);
        QVERIFY2(out.contains("To copy:") && out.contains("Dry run: nothing was copied."), qPrintable(out));
        QVERIFY(QDir(s.prefix).isEmpty());
        QCOMPARE(run(base, &out, &err), 2); QVERIFY2(err.contains("--yes"), qPrintable(err));
        QVERIFY(QDir(s.prefix).isEmpty());
        QCOMPARE(run(base + QStringList{"--yes"}, &out, &err), 0); QVERIFY2(out.contains("Imported"), qPrintable(out));
        QCOMPARE(slurp(s.prefix + "/" + Priv + "/Resources/p.txt"), QByteArray("private resource"));
        QCOMPARE(run(base + QStringList{"--yes"}, &out, &err), 0); QVERIFY2(out.contains("Already present"), qPrintable(out));
        QVERIFY(QDir().mkpath(s.prefix + "/inside"));
        QCOMPARE(run({"--volume", s.prefix, "--prefix", s.prefix + "/inside", "--yes"}, &out, &err), 1); // nested locations
    }
};
QTEST_APPLESS_MAIN(FrameworkResourcesTest)
#include "frameworkresources_test.moc"
