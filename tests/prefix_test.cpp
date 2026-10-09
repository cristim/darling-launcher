// SPDX-License-Identifier: GPL-3.0-or-later
#include "prefixbuilder.h"
#include "discovery.h"
#include "releases.h"
#include "log.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>
#include <sys/stat.h>
#include <unistd.h>

namespace {
QJsonObject readJson(const QString &path) { QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return QJsonDocument::fromJson(file.readAll()).object(); }
QString sha256File(const QString &path) { QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {}; return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex(); }
// Python's tarfile can write member names and owner names that GNU tar would not produce itself.
const char *archiveWriter = R"(
import io, json, sys, tarfile
out, spec = sys.argv[1], json.loads(sys.argv[2])
with tarfile.open(out, "w", format=tarfile.GNU_FORMAT) as archive:
    for member in spec:
        info = tarfile.TarInfo(member["name"]); info.mtime = 1760000000; info.uname = member.get("uname", "u"); info.gname = member.get("gname", "g")
        info.mode = int(member.get("mode", "644"), 8); data = b""
        if member.get("type") == "dir": info.type = tarfile.DIRTYPE; info.mode = 0o755
        elif member.get("type") == "link": info.type = tarfile.SYMTYPE; info.linkname = member["target"]
        else: data = member.get("data", "x").encode(); info.size = len(data)
        archive.addfile(info, io.BytesIO(data))
)";
// Writes <dir>/<name>.tar.zst; members are {name, type: file|dir|link, target, gname, mode}.
QString makeArchive(const QString &dir, const QString &name, const QJsonArray &members) {
    const QString tar = dir + "/" + name + ".tar", zst = tar + ".zst";
    if (QProcess::execute("python3", {"-c", archiveWriter, tar, QJsonDocument(members).toJson(QJsonDocument::Compact)}) != 0) return {};
    if (QProcess::execute("zstd", {"-q", "-f", "--rm", tar, "-o", zst}) != 0) return {};
    return zst;
}
QJsonObject dirMember(const QString &name) { return {{"name", name}, {"type", "dir"}}; }
QJsonObject fileMember(const QString &name) { return {{"name", name}}; }
QJsonObject linkMember(const QString &name, const QString &target) { return {{"name", name}, {"type", "link"}, {"target", target}}; }
// The skeleton every installable fixture needs.
QJsonArray runtimeMembers() {
    QJsonArray members;
    for (const char *dir : {"usr", "usr/local", "usr/local/bin", "usr/local/libexec", "usr/local/libexec/darling", "usr/local/libexec/darling/private", "usr/local/libexec/darling/private/etc"}) members.append(dirMember(dir));
    members.append(fileMember("usr/local/bin/darling")); return members;
}
// Stand-ins for a host /usr/local runtime: <root>/hostlocal and <root>/hostroot/local, each with bin/darling and the marker.
bool makeHostRuntimes(const QString &root) {
    for (const QString &host : {root + "/hostroot/local", root + "/hostlocal"}) {
        if (!QDir().mkpath(host + "/bin") || !QDir().mkpath(host + "/libexec/darling/private/etc")) return false;
        QFile launcher(host + "/bin/darling"); if (!launcher.open(QIODevice::WriteOnly)) return false; launcher.close();
        if (!launcher.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) return false;
    }
    return true;
}
QString writeScript(const QString &path, const QString &body) {
    QFile f(path); if (!f.open(QIODevice::WriteOnly)) return {};
    f.write(("#!/bin/sh\n" + body + "\n").toUtf8()); f.close();
    return f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner) ? path : QString();
}
}
class PrefixTest : public QObject {
    Q_OBJECT
    QTemporaryDir isolatedHome;
private slots:
    void logRecordsBuilderOutputUnderDataFolder() {
        LauncherLog::write("test", "first line\nsecond line");
        QCOMPARE(LauncherLog::path(), QDir::homePath() + "/.darling-launcher/logs/launcher.log");
        QFile log(LauncherLog::path()); QVERIFY(log.open(QIODevice::ReadOnly)); const QString text = QString::fromUtf8(log.readAll());
        QVERIFY(text.contains("[test] first line")); QVERIFY(text.contains("[test] second line"));
    }
    void initTestCase() { QVERIFY(isolatedHome.isValid()); qputenv("HOME", isolatedHome.path().toUtf8()); qputenv("DARLING_LAUNCHER_LOCK_DIR", (isolatedHome.path() + "/locks").toUtf8()); }
    void independentClone() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QString git = QStandardPaths::findExecutable("git"); QVERIFY(!git.isEmpty());
        QString original = temporary.path() + "/source", destination = temporary.path() + "/clone with spaces";
        QCOMPARE(QProcess::execute(git, {"init", original}), 0);
        QProcess clone;
        clone.start(git, LauncherDiscovery::cloneArguments(original, destination));
        QVERIFY(clone.waitForFinished()); QCOMPARE(clone.exitCode(), 0);
        QVERIFY(QFileInfo::exists(destination + "/.git"));
        clone.start(git, LauncherDiscovery::cloneArguments(original, destination));
        QVERIFY(clone.waitForFinished()); QVERIFY(clone.exitCode() != 0);
        QVERIFY(QFileInfo::exists(original + "/.git"));
    }
    void runtimeArchiveInstall() {
        using namespace LauncherReleases;
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); const QString root = temporary.path();
        auto run = [&](const QStringList &args) { QProcess p; p.setWorkingDirectory(root); p.start(args.first(), args.mid(1)); QVERIFY2(p.waitForFinished(30000) && p.exitCode() == 0, qPrintable(p.readAllStandardError())); };
        auto digest = [](const QString &path) { QFile f(path); f.open(QIODevice::ReadOnly); return QString(QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex()); };
        QVERIFY(QDir().mkpath(root + "/good/usr/local/bin")); QVERIFY(QDir().mkpath(root + "/good/usr/local/libexec/darling/private/etc")); QVERIFY(QDir().mkpath(root + "/evil"));
        { QFile f(root + "/good/usr/local/bin/darling"); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("x"); }
        QVERIFY(QFile::link("/usr/lib/libz.so", root + "/good/usr/local/libz"));
        run({"tar", "--zstd", "-cf", "good.tar.zst", "-C", "good", "usr"});
        QVERIFY(QFile::link("/etc", root + "/evil/dir")); { QFile f(root + "/evil/payload"); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("x"); }
        run({"tar", "--zstd", "-cf", "through.tar.zst", "-C", "evil", "--transform", "s,^dir,usr/local/dir,", "--transform", "s,^payload,usr/local/dir/payload,", "dir", "payload"});
        run({"tar", "--zstd", "-cf", "escape.tar.zst", "-C", "evil", "--transform", "s,^payload,../payload,", "payload"});
        const QString link = "lrwxrwxrwx 0/0 0 2026-10-09 14:32 usr/local/dir -> /etc", file = "-rw-r--r-- 0/0 1 2026-10-09 14:32 ";
        for (const char *path : {"usr/local/dir/payload", "./usr/local/dir/./payload", "usr/local/dir//payload"}) QVERIFY2(vetMembers({link, file + path}).contains("writes through the symlink"), path);
        QVERIFY(vetMembers({link, file + "usr/local/dir/sub/../payload"}).contains("escapes the runtime folder"));
        QVERIFY(vetMembers({link, file + "usr/local/other/payload"}).isEmpty());
        QVERIFY(vetMembers({"lrwxrwxrwx 0/0 0 2026-10-09 14:32 usr/local/a -> b -> /home/u/.ssh", file + "usr/local/a -> b/authorized_keys"}).contains("' -> '"));
        QVERIFY(vetMembers({file + "usr/local/a -> b"}).contains("' -> '"));
        QVERIFY(!vetMembers({file + "a\\nb"}).isEmpty());
        auto script = [&](const QString &name, const QString &body) { QFile f(root + "/" + name); f.open(QIODevice::WriteOnly); f.write(("#!/bin/sh\n" + body + "\n").toUtf8()); f.close(); f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner); return f.fileName(); };
        const QString ghOk = script("gh-ok", "echo \"$@\" > '" + root + "/gh-args'"), ghBad = script("gh-bad", "echo untrusted >&2; exit 1"), repo = "VibeDarling/darling";
        const QString tag = "v2026.10.09-3721b65", good = root + "/good.tar.zst", runtimes = root + "/runtimes";
        QVERIFY(QDir().mkpath(root + "/linked/usr/local/bin")); QVERIFY(QFile::link("/usr/local/bin/darling", root + "/linked/usr/local/bin/darling"));
        run({"tar", "--zstd", "-cf", "linked.tar.zst", "-C", "linked", "usr"});
        const QString linked = root + "/linked.tar.zst";
        QVERIFY(installArchive(linked, digest(linked), runtimes, "v2026.10.09-aaaaaaa", ghOk, repo).contains("runtime layout"));
        QVERIFY(installArchive(good, digest(good), runtimes, "latest", ghOk, repo).contains("not a release tag"));
        QVERIFY2(installArchive(good, QString(64, '0'), runtimes, tag, ghOk, repo).contains("SHA-256 mismatch"), "digest");
        QVERIFY(installArchive(good, digest(good), runtimes, tag, ghBad, repo).contains("attestation verification failed"));
        QVERIFY(installArchive(good, digest(good), runtimes, tag, {}, repo).contains("gh is required"));
        QVERIFY(!QFileInfo::exists(runtimes + "/" + tag));
        QVERIFY(!QFileInfo::exists(runtimes + "/" + tag));
        for (const char *name : {"through", "escape"}) { const QString archive = root + "/" + name + ".tar.zst"; const QString error = installArchive(archive, digest(archive), runtimes, tag, ghOk, repo); QVERIFY2(!error.isEmpty() && (error.contains("symlink") || error.contains("escapes")), name); QVERIFY(!QFileInfo::exists(runtimes + "/" + tag)); }
        QCOMPARE(installArchive(good, digest(good), runtimes, tag, ghOk, repo), QString());
        QVERIFY(QFileInfo(runtimes + "/" + tag + "/usr/local/bin/darling").isFile());
        QVERIFY(QFile(root + "/gh-args").open(QIODevice::ReadOnly)); { QFile args(root + "/gh-args"); args.open(QIODevice::ReadOnly); QVERIFY(args.readAll().contains("--signer-workflow VibeDarling/darling/.github/workflows/release-binaries.yml")); }
        QVERIFY(QFileInfo(runtimes + "/" + tag + "/usr/local/libz").isSymLink());
        QVERIFY(installArchive(good, digest(good), runtimes, tag, ghOk, repo).contains("already exists"));
    }
    // Review F1: tar prints uname/gname unescaped, so a gname shaped like "<size> <date> <time> <name>" shifted
    // the parsed path and hid an absolute member and a write through a symlink from vetMembers.
    void ownerNameSpoofIsRefused() {
        using namespace LauncherReleases;
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); const QString root = temporary.path(), runtimes = root + "/runtimes";
        const QString gh = writeScript(root + "/gh-ok", "exit 0"); QVERIFY(!gh.isEmpty());
        const QString spoof = "g 1 2026-10-09 14:32 usr/zz";
        QJsonObject absolute = fileMember(root + "/outside/pwn-abs"); absolute.insert("gname", spoof);
        QJsonObject through = fileMember("usr/local/a/pwn-rel"); through.insert("gname", spoof);
        QJsonArray absMembers = runtimeMembers(), chainMembers = runtimeMembers();
        absMembers.append(absolute);
        chainMembers.append(dirMember("usr/local/share")); chainMembers.append(linkMember("usr/local/a", "share")); chainMembers.append(through);
        const QString abs = makeArchive(root, "gnameabs", absMembers), chain = makeArchive(root, "relchain", chainMembers);
        QVERIFY(!abs.isEmpty()); QVERIFY(!chain.isEmpty());
        const QString absError = installArchive(abs, sha256File(abs), runtimes, "v2026.10.09-0000016", gh, "VibeDarling/darling");
        QVERIFY2(absError.contains("escapes the runtime folder"), qPrintable(absError));
        const QString chainError = installArchive(chain, sha256File(chain), runtimes, "v2026.10.09-0000018", gh, "VibeDarling/darling");
        QVERIFY2(chainError.contains("writes through the symlink usr/local/a"), qPrintable(chainError));
        QVERIFY(!QFileInfo::exists(runtimes + "/v2026.10.09-0000016")); QVERIFY(!QFileInfo::exists(runtimes + "/v2026.10.09-0000018"));
        QVERIFY(vetMembers({"-rw-r--r-- u/" + spoof + " 1 2025-10-09 10:53 /abs"}).contains("unparsable"));
    }
    // Security S5: CI packs only usr/local/**, so anything else in an archive is refused before extraction.
    void membersOutsideUsrLocalAreRefused() {
        using namespace LauncherReleases;
        const QString dir = "drwxr-xr-x 0/0 0 2026-10-09 14:32 ", file = "-rw-r--r-- 0/0 1 2026-10-09 14:32 ";
        QVERIFY(vetMembers({dir + "usr/", dir + "usr/local/", dir + "usr/local/bin/", file + "usr/local/bin/darling", ""}).isEmpty());
        for (const char *path : {"etc/passwd", "usr/lib/libz.so", "usr/localx/file", "./opt/x", "usr/local/../lib/x"}) QVERIFY2(!vetMembers({file + path}).isEmpty(), path);
        for (const char *path : {"etc/passwd", "usr/lib/libz.so", "usr/localx/file", "./opt/x"}) QVERIFY2(vetMembers({file + path}).contains("outside usr/local"), path);
    }
    // Review F2: a symlinked parent of usr/local/bin/darling passed vetting (no member below the link) and
    // the launcher then resolved to a host file, e.g. the setuid /usr/local/bin/darling.
    void symlinkedRuntimeParentsAreRefused() {
        using namespace LauncherReleases;
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); const QString root = temporary.path(), runtimes = root + "/runtimes";
        const QString gh = writeScript(root + "/gh-ok", "exit 0"); QVERIFY(!gh.isEmpty());
        QVERIFY(makeHostRuntimes(root));
        const struct { const char *name, *tag; QJsonArray members; } cases[] = {
            {"parentlink", "v2026.10.09-0000008", {dirMember("usr"), linkMember("usr/local", root + "/hostlocal")}},
            {"usrlink", "v2026.10.09-0000009", {linkMember("usr", root + "/hostroot")}},
            {"binlink", "v2026.10.09-000000a", {dirMember("usr"), dirMember("usr/local"), linkMember("usr/local/bin", "../../../../hostlocal/bin")}},
            {"etclink", "v2026.10.09-000000b", {dirMember("usr"), dirMember("usr/local"), dirMember("usr/local/bin"), fileMember("usr/local/bin/darling"), dirMember("usr/local/libexec"), linkMember("usr/local/libexec/darling", root + "/hostlocal/libexec/darling")}},
        };
        for (const auto &shape : cases) {
            const QString archive = makeArchive(root, shape.name, shape.members); QVERIFY(!archive.isEmpty());
            const QString error = installArchive(archive, sha256File(archive), runtimes, shape.tag, gh, "VibeDarling/darling");
            QVERIFY2(error.contains("runtime layout"), qPrintable(QString(shape.name) + ": " + error));
            QVERIFY2(!QFileInfo::exists(runtimes + "/" + shape.tag), shape.name);
        }
    }
    void symlinkedRuntimeParentsAreNotDiscovered() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); qputenv("HOME", temporary.path().toUtf8());
        const QString root = temporary.path(), runtimes = LauncherDiscovery::dataRoot() + "/runtimes";
        QVERIFY(makeHostRuntimes(root));
        QVERIFY(QDir().mkpath(runtimes + "/v2026.10.09-0000008/usr")); QVERIFY(QFile::link(root + "/hostlocal", runtimes + "/v2026.10.09-0000008/usr/local"));
        QVERIFY(QDir().mkpath(runtimes + "/v2026.10.09-0000009")); QVERIFY(QFile::link(root + "/hostroot", runtimes + "/v2026.10.09-0000009/usr"));
        const QString bin = runtimes + "/v2026.10.09-000000a/usr/local"; QVERIFY(QDir().mkpath(bin + "/libexec/darling/private/etc")); QVERIFY(QFile::link(root + "/hostlocal/bin", bin + "/bin"));
        const QString etc = runtimes + "/v2026.10.09-000000b/usr/local"; QVERIFY(QDir().mkpath(etc + "/bin")); QVERIFY(QDir().mkpath(etc + "/libexec"));
        QVERIFY(QFile::copy(root + "/hostlocal/bin/darling", etc + "/bin/darling")); QVERIFY(QFile::link(root + "/hostlocal/libexec/darling", etc + "/libexec/darling"));
        const QString good = runtimes + "/v2026.10.09-000000c/usr/local"; QVERIFY(QDir().mkpath(good + "/bin")); QVERIFY(QDir().mkpath(good + "/libexec/darling/private/etc"));
        QVERIFY(QFile::copy(root + "/hostlocal/bin/darling", good + "/bin/darling"));
        for (const char *tag : {"0000008", "0000009", "000000a", "000000b"}) {
            const QString runtime = runtimes + "/v2026.10.09-" + tag + "/usr/local";
            QVERIFY2(QFileInfo(runtime + "/bin/darling").isExecutable() && QFileInfo(runtime + "/libexec/darling/private/etc").isDir(), tag);
        }
        const auto found = LauncherDiscovery::runtimes({}, {});
        qputenv("HOME", isolatedHome.path().toUtf8());
        QCOMPARE(found.size(), 1); QCOMPARE(found.first().launcher, good + "/bin/darling");
    }
    // Security view property 1: what tar wrote is checked again, so a listing-parser gap cannot reach discovery.
    void extractedTreeIsVerified() {
        using namespace LauncherReleases;
        auto runtime = [](const QString &dir) {
            if (!QDir().mkpath(dir + "/usr/local/bin") || !QDir().mkpath(dir + "/" + runtimeMarker)) return false;
            QFile launcher(dir + "/" + launcherPath); return launcher.open(QIODevice::WriteOnly);
        };
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); const QString root = temporary.path();
        QVERIFY(runtime(root + "/ok")); QVERIFY(QFile::link("/System/Library/Frameworks", root + "/ok/usr/local/abs-link"));
        QCOMPARE(extractedTreeError(root + "/ok"), QString());
        QVERIFY(runtime(root + "/fifo")); QCOMPARE(::mkfifo(QFile::encodeName(root + "/fifo/usr/local/p").constData(), 0600), 0);
        QVERIFY(extractedTreeError(root + "/fifo").contains("not a file, folder or symlink"));
        QVERIFY(runtime(root + "/hard")); QCOMPARE(::link(QFile::encodeName(root + "/hard/" + launcherPath).constData(), QFile::encodeName(root + "/hard/usr/local/h").constData()), 0);
        QVERIFY(extractedTreeError(root + "/hard").contains("hard link"));
        QVERIFY(runtime(root + "/suid")); QCOMPARE(::chmod(QFile::encodeName(root + "/suid/" + launcherPath).constData(), 04755), 0);
        QVERIFY(extractedTreeError(root + "/suid").contains("setuid"));
        QVERIFY(runtime(root + "/hidden")); QVERIFY(QDir().mkpath(root + "/hidden/usr/local/h/x"));
        QCOMPARE(::mkfifo(QFile::encodeName(root + "/hidden/usr/local/h/x/p").constData(), 0600), 0); QCOMPARE(::chmod(QFile::encodeName(root + "/hidden/usr/local/h").constData(), 0), 0);
        const QString hiddenError = extractedTreeError(root + "/hidden");
        QCOMPARE(::chmod(QFile::encodeName(root + "/hidden/usr/local/h").constData(), 0700), 0);
        QVERIFY(hiddenError.contains("not readable, writable and searchable"));
        for (const char *outside : {"/etc", "/usr/lib"}) {
            const QString dir = root + "/outside" + QString(outside).replace('/', '-'); QVERIFY(runtime(dir)); QVERIFY(QDir().mkpath(dir + outside));
            QVERIFY2(extractedTreeError(dir).contains("outside usr/local"), outside);
        }
        QVERIFY(runtime(root + "/host")); QVERIFY(QDir().mkpath(root + "/split/usr")); QVERIFY(QFile::link(root + "/host/usr/local", root + "/split/usr/local"));
        QVERIFY(extractedTreeError(root + "/split").contains("leaves the runtime folder"));
    }
    // Review F4: the signer pin followed the repo argument, so a fork override also replaced the trust root.
    void signerPinIgnoresRepository() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); const QString root = temporary.path();
        const QString gh = writeScript(root + "/gh", "echo \"$@\" > '" + root + "/gh-args'"); QVERIFY(!gh.isEmpty());
        QCOMPARE(LauncherReleases::verifyAttestation(gh, root + "/archive.tar.zst", "cristim/darling"), QString());
        QFile args(root + "/gh-args"); QVERIFY(args.open(QIODevice::ReadOnly));
        QCOMPARE(args.readAll().trimmed(), QByteArray("attestation verify " + (root + "/archive.tar.zst").toUtf8() + " --repo cristim/darling --signer-workflow VibeDarling/darling/.github/workflows/release-binaries.yml --source-ref refs/heads/master --deny-self-hosted-runners"));
    }
    // Review F5: two installs of one tag could both pass exists(staging) before either mkpath()ed it; the loser's
    // tar then kept writing into the folder the winner had already renamed into place.
    void concurrentSameTagInstallsAreExclusive() {
        using namespace LauncherReleases;
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); const QString root = temporary.path();
        const QString gh = writeScript(root + "/gh", "exit 0"), archive = makeArchive(root, "good", runtimeMembers()), realTar = QStandardPaths::findExecutable("tar");
        QVERIFY(!gh.isEmpty()); QVERIFY(!archive.isEmpty()); QVERIFY(!realTar.isEmpty()); QVERIFY(QDir().mkpath(root + "/bin"));
        // Listings wait for each other so both installs reach the staging claim together; extractions are counted and slowed.
        QVERIFY(!writeScript(root + "/bin/tar", "case \"$*\" in\n"
            "*-tvf*) \"" + realTar + "\" \"$@\" || exit $?; : > \"$BARRIER/$$\"; i=0\n"
            "  while [ $i -lt 500 ]; do set -- \"$BARRIER\"/*; [ $# -ge 2 ] && exit 0; i=$((i+1)); sleep 0.01; done; exit 0;;\n"
            "*-xf*) echo x >> \"$BARRIER/../extracts\"; sleep 0.3;;\nesac\nexec \"" + realTar + "\" \"$@\"").isEmpty());
        const QByteArray path = qgetenv("PATH");
        struct Restore { QByteArray path; ~Restore() { qputenv("PATH", path); qunsetenv("BARRIER"); } } restore{path};
        qputenv("PATH", (root + "/bin:").toUtf8() + path);
        const QString digest = sha256File(archive), tag = "v2026.10.09-0000005";
        int doubleExtractions = 0, wrongWinners = 0, unclearLosers = 0;
        for (int round = 0; round < 20; ++round) {
            const QString work = root + "/round" + QString::number(round), runtimes = work + "/runtimes";
            QVERIFY(QDir().mkpath(work + "/barrier")); qputenv("BARRIER", (work + "/barrier").toUtf8());
            QString first, second;
            QThread *a = QThread::create([&] { first = installArchive(archive, digest, runtimes, tag, gh, "VibeDarling/darling"); });
            QThread *b = QThread::create([&] { second = installArchive(archive, digest, runtimes, tag, gh, "VibeDarling/darling"); });
            a->start(); b->start(); const bool finished = a->wait(60000) && b->wait(60000); a->wait(); b->wait(); delete a; delete b; QVERIFY(finished);
            QFile log(work + "/extracts"); QVERIFY(log.open(QIODevice::ReadOnly));
            if (log.readAll().count('x') != 1) ++doubleExtractions;
            if (first.isEmpty() == second.isEmpty()) ++wrongWinners;
            const QString loser = first.isEmpty() ? second : first;
            if (!loser.contains("another install of " + tag)) { ++unclearLosers; qInfo() << "round" << round << "loser:" << loser; }
            QVERIFY(QFileInfo(runtimes + "/" + tag + "/" + launcherPath).isFile()); QVERIFY(!QFileInfo::exists(runtimes + "/" + tag + ".partial"));
        }
        qInfo() << "rounds with two extractions:" << doubleExtractions << "without exactly one winner:" << wrongWinners << "loser not told about the other install:" << unclearLosers;
        QCOMPARE(doubleExtractions, 0); QCOMPARE(wrongWinners, 0); QCOMPARE(unclearLosers, 0);
    }
    void releaseTagsAndSelection() {
        using namespace LauncherReleases;
        for (const char *good : {"v2026.10.09-3721b65", "v2026.10.09-3721b65-r2"}) QVERIFY2(validTag(good), good);
        for (const char *bad : {"2026-10-09-14-32-3721b65", "v2026.10.09-3721b6", "v2026.10.09-3721b65.partial", "v2026.10.09-XYZ1234", "latest", "../v2026.10.09-3721b65"}) QVERIFY2(!validTag(bad), bad);
        QVERIFY(!hostArchitecture().isEmpty());
        const QString tag = "v2026.10.09-3721b65", file = "darling-runtime-" + tag + "-linux-aarch64.tar.zst", url = "https://github.com/VibeDarling/darling/releases/download/" + tag + "/" + file;
        const QJsonObject entry{{"file", file}, {"url", url}, {"sha256", QString(64, 'a')}, {"size", 1}, {"unpacked_size", 2}, {"install_root", "usr/local"}, {"launcher", "usr/local/bin/darling"}};
        const QJsonObject manifest{{"schema", 1}, {"version", tag}, {"artifacts", QJsonObject{{"aarch64", entry}}}};
        const QJsonObject release{{"tag_name", tag}, {"assets", QJsonArray{QJsonObject{{"name", file}, {"browser_download_url", url}, {"digest", "sha256:" + QString(64, 'a')}}}}};
        auto chosen = select(release, manifest, "aarch64", false); QVERIFY2(chosen.valid(), qPrintable(chosen.error));
        QCOMPARE(chosen.tag, tag); QCOMPARE(chosen.artifact.sha256, QString(64, 'a')); QCOMPARE(chosen.artifact.url, url);
        auto with = [](QJsonObject object, const QString &key, const QJsonValue &value) { object.insert(key, value); return object; };
        auto entryWith = [&](const QString &key, const QJsonValue &value) { return with(manifest, "artifacts", QJsonObject{{"aarch64", with(entry, key, value)}}); };
        QVERIFY(select(release, manifest, "x86_64", false).error.contains("no runtime for x86_64"));
        QVERIFY(select(with(release, "prerelease", true), manifest, "aarch64", false).error.contains("prerelease"));
        QVERIFY(select(with(release, "prerelease", true), manifest, "aarch64", true).valid());
        QVERIFY(select(with(release, "draft", true), manifest, "aarch64", true).error.contains("draft"));
        QVERIFY(select(release, with(manifest, "schema", 2), "aarch64", false).error.contains("schema"));
        QVERIFY(select(release, with(manifest, "version", "v2026.10.08-aaaaaaa"), "aarch64", false).error.contains("does not match"));
        QVERIFY(select(release, entryWith("install_root", "opt"), "aarch64", false).error.contains("install_root"));
        QVERIFY(select(release, entryWith("launcher", "usr/local/bin/other"), "aarch64", false).error.contains("install_root"));
        QVERIFY(select(release, entryWith("file", "other.tar.zst"), "aarch64", false).error.contains("file name"));
        QVERIFY(select(release, entryWith("sha256", "bb"), "aarch64", false).error.contains("SHA-256"));
        QVERIFY(select(release, entryWith("sha256", QString(64, 'b')), "aarch64", false).error.contains("differs from the GitHub digest"));
        QVERIFY(select(release, entryWith("url", "https://evil.invalid/x"), "aarch64", false).error.contains("url differs"));
        QVERIFY(select(with(release, "assets", QJsonArray{}), manifest, "aarch64", false).error.contains("does not list"));
        QVERIFY(select(with(release, "tag_name", "2026-10-09-14-32-3721b65"), manifest, "aarch64", false).error.contains("not vYYYY"));
    }
    void prebuiltRuntimesAreDiscoveredByTag() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); qputenv("HOME", temporary.path().toUtf8());
        const QString tag = LauncherDiscovery::dataRoot() + "/runtimes/v2026.10.09-3721b65";
        QVERIFY(QDir().mkpath(tag + "/usr/local/bin")); QVERIFY(QDir().mkpath(tag + "/usr/local/libexec/darling/private/etc"));
        QFile launcher(tag + "/usr/local/bin/darling"); QVERIFY(launcher.open(QIODevice::WriteOnly)); launcher.close(); QVERIFY(launcher.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto found = LauncherDiscovery::runtimes({}, {}); QCOMPARE(found.size(), 1);
        QCOMPARE(found.first().launcher, launcher.fileName()); QCOMPARE(found.first().installRoot, tag + "/usr/local");
        const QString partial = tag + ".partial"; QVERIFY(QDir().mkpath(partial + "/usr/local/bin")); QVERIFY(QDir().mkpath(partial + "/usr/local/libexec/darling/private/etc"));
        QVERIFY(QFile::copy(launcher.fileName(), partial + "/usr/local/bin/darling")); QCOMPARE(LauncherDiscovery::runtimes({}, {}).size(), 1);
        QVERIFY(QFile::remove(launcher.fileName())); QVERIFY(QFile::link("/bin/true", launcher.fileName())); QVERIFY(LauncherDiscovery::runtimes({}, {}).isEmpty());
    }
    void runtimeDefaults() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QString root = temporary.path(), install = root + "/installed";
        QVERIFY(QDir().mkpath(install + "/bin"));
        QFile launcher(install + "/bin/darling"); QVERIFY(launcher.open(QIODevice::WriteOnly)); launcher.close();
        QVERIFY(launcher.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        QVERIFY(LauncherDiscovery::runtimes({root}, launcher.fileName()).isEmpty());
        QVERIFY(QDir().mkpath(install + "/libexec/darling/private/etc"));
        auto found = LauncherDiscovery::runtimes({root}, launcher.fileName()); QCOMPARE(found.size(), 1);
        QCOMPARE(found.first().launcher, launcher.fileName()); QCOMPARE(found.first().installRoot, install);
        QString built = root + "/darling-workspace";
        QVERIFY(QDir().mkpath(built + "/build/src/startup"));
        QVERIFY(QDir().mkpath(built + "/image/usr/local/libexec/darling/private/etc"));
        QVERIFY(QFile::copy(launcher.fileName(), built + "/build/src/startup/darling"));
        QCOMPARE(LauncherDiscovery::runtimes({root}, launcher.fileName()).size(), 2);
        QCOMPARE(LauncherDiscovery::managedPrefix(root), root + "/prefixes/default");
        QVERIFY(QDir().mkpath(built + "/build"));
        QVERIFY(QFile::copy(launcher.fileName(), built + "/build/launcher-fixture-helper"));
        QCOMPARE(LauncherDiscovery::helperExecutable({root}, "launcher-fixture-helper"), built + "/build/launcher-fixture-helper");
        QVERIFY(QDir().mkpath(root + "/apfs-fuse/build"));
        QVERIFY(QFile::copy(launcher.fileName(), root + "/apfs-fuse/build/launcher-fixture-helper"));
        QCOMPARE(LauncherDiscovery::helperExecutable({root}, "launcher-fixture-helper"), root + "/apfs-fuse/build/launcher-fixture-helper");
    }
    void cleanBuildSource() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); QString root = temporary.path();
        QString source = root + "/darling-source", git = QStandardPaths::findExecutable("git");
        QCOMPARE(QProcess::execute(git, {"init", "-b", "main", source}), 0);
        QVERIFY(QDir().mkpath(source + "/src/startup"));
        QFile cmake(source + "/CMakeLists.txt"); QVERIFY(cmake.open(QIODevice::WriteOnly)); cmake.write("fixture"); cmake.close();
        QCOMPARE(QProcess::execute(git, {"-C", source, "add", "."}), 0);
        QCOMPARE(QProcess::execute(git, {"-C", source, "-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-m", "fixture"}), 0);
        QCOMPARE(QProcess::execute(git, {"-C", source, "remote", "add", "origin", "https://github.com/VibeDarling/darling.git"}), 0);
        QCOMPARE(LauncherDiscovery::cleanSource({root}), source);
        QVERIFY(cmake.open(QIODevice::Append)); cmake.write("dirty"); cmake.close();
        QVERIFY(LauncherDiscovery::cleanSource({root}).isEmpty());
    }
    void volumeDefaults() {
        QCOMPARE(LauncherDiscovery::defaultVolume({"/mounted/macOS"}, ""), QString("/mounted/macOS"));
        QCOMPARE(LauncherDiscovery::defaultVolume({}, ""), QString());
        QCOMPARE(LauncherDiscovery::defaultVolume({"/one", "/two"}, "/two"), QString("/two"));
    }
    void pathDiscovery() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QString root = temporary.path();
        QVERIFY(QDir().mkpath(root + "/vibedarling/tools"));
        QVERIFY(QDir().mkpath(root + "/vibedarling/.git"));
        QVERIFY(QDir().mkpath(root + "/vibedarling/src/startup"));
        QFile script(root + "/vibedarling/tools/all-vibedarling-pr-prefix.py"); QVERIFY(script.open(QIODevice::WriteOnly)); script.close();
        QFile cmake(root + "/vibedarling/CMakeLists.txt"); QVERIFY(cmake.open(QIODevice::WriteOnly)); cmake.close();
        QCOMPARE(LauncherDiscovery::scripts({root}), QStringList{script.fileName()});
        QCOMPARE(LauncherDiscovery::sources({root}), QStringList{root + "/vibedarling"});
        QCOMPARE(LauncherDiscovery::newWorkspace(root), root + "/darling-workspace");
        QVERIFY(QDir().mkpath(root + "/darling-workspace"));
        QCOMPARE(LauncherDiscovery::newWorkspace(root), root + "/darling-workspace-2");
        QVERIFY(LauncherDiscovery::sources({root + "/missing"}).isEmpty());
    }
    void preserveDiscoveryInputs() {
        QJsonObject discovery{{"schema", 1}, {"owner", "VibeDarling"}, {"complete", true},
            {"repos", QJsonArray{QJsonObject{{"repo", "darling"}, {"branch", "master"}, {"base", QString(40, 'a')}, {"prs", QJsonArray{QJsonObject{{"number", 7}}}}}}}};
        QJsonObject selected; QString error;
        QVERIFY(LauncherPrefix::selectInputs(discovery, &selected, &error));
        QVERIFY(selected.value("repos").toArray().first().toObject().value("prs").toArray().isEmpty());
        QCOMPARE(discovery.value("repos").toArray().first().toObject().value("prs").toArray().size(), 1);
        discovery.insert("complete", false); QVERIFY(!LauncherPrefix::selectInputs(discovery, &selected, &error));
    }
    void externalBuilder_data() {
        QTest::addColumn<bool>("nested");
        QTest::newRow("default-branches-legacy") << false; QTest::newRow("default-branches-nested") << true;
    }
    void externalBuilder() {
        QFETCH(bool, nested);
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QString source = temporary.path() + "/source", workspace = temporary.path() + "/new workspace";
        QVERIFY(QDir().mkpath(source));
        QString script = QString(TEST_SOURCE_DIR) + "/tests/fixtures/prefix_builder.py";
        if (!nested) {
            QFile input(script); QVERIFY(input.open(QIODevice::ReadOnly)); auto bytes = input.readAll(); bytes.replace(", \"resolve-nested\", \"checkout-nested\"", "");
            script = temporary.path() + "/legacy.py"; QFile legacy(script); QVERIFY(legacy.open(QIODevice::WriteOnly)); legacy.write(bytes); legacy.close();
        }
        PrefixBuildRequest request{QStandardPaths::findExecutable("python3"), script, source, workspace, 2, {"-DENABLE_TESTS=ON"}};
        PrefixBuilder builder; QSignalSpy completed(&builder, &PrefixBuilder::completed), phases(&builder, &PrefixBuilder::phaseChanged);
        builder.start(request); QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 5000);
        QVERIFY2(completed.first()[0].toBool(), qPrintable(completed.first()[1].toString())); QCOMPARE(phases.size(), nested ? 6 : 4);
        QCOMPARE(completed.first()[2].toString(), workspace + "/prefix");
        auto discovery = readJson(workspace + "/refs.discovery.lock.json"), selected = readJson(workspace + "/refs.lock.json");
        QCOMPARE(discovery.value("repos").toArray().first().toObject().value("prs").toArray().size(), 1);
        QCOMPARE(selected.value("repos").toArray().first().toObject().value("prs").toArray().size(), 0);
        auto provenance = readJson(workspace + "/prefix/.darling-launcher/build-provenance.json");
        QCOMPARE(provenance.value("script_sha256").toString().size(), 64);
        if (nested) {
            auto lock = readJson(workspace + "/nested.refs.lock.json"), original = readJson(workspace + "/nested.discovery.lock.json");
            QCOMPARE(lock.value("vibedarling").toArray().first().toObject().value("prs").toArray().size(), 0);
            QCOMPARE(original.value("vibedarling").toArray().first().toObject().value("prs").toArray().size(), 1);
        }
        QCOMPARE(readJson(workspace + "/build-call.json").value("cmake_args").toArray(), QJsonArray{"-DENABLE_TESTS=ON"});
        QVERIFY(QDir(source).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    }
    void stopOnIntegrationFailure() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QString source = temporary.path() + "/source"; QVERIFY(QDir().mkpath(source));
        QFile marker(source + "/fail-checkout"); QVERIFY(marker.open(QIODevice::WriteOnly)); marker.close();
        PrefixBuilder builder; QSignalSpy completed(&builder, &PrefixBuilder::completed), phases(&builder, &PrefixBuilder::phaseChanged);
        builder.start({QStandardPaths::findExecutable("python3"), QString(TEST_SOURCE_DIR) + "/tests/fixtures/prefix_builder.py", source, temporary.path() + "/new", 1, {}});
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 5000); QVERIFY(!completed.first()[0].toBool()); QCOMPARE(phases.size(), 3);
        QVERIFY(!QFileInfo::exists(temporary.path() + "/new/build-call.json"));
    }
    void refuseExistingOrSharedWorkspace() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QString source = temporary.path() + "/source"; QVERIFY(QDir().mkpath(source));
        PrefixBuildRequest request{QStandardPaths::findExecutable("python3"), QString(TEST_SOURCE_DIR) + "/tests/fixtures/prefix_builder.py", source, source, 1, {}}; QString error;
        QVERIFY(!LauncherPrefix::validateRequest(request, &error));
        request.workspace = source + "/new"; QVERIFY(!LauncherPrefix::validateRequest(request, &error));
        request.workspace = temporary.path() + "/new"; request.cmakeArguments = {"shell-command"}; QVERIFY(!LauncherPrefix::validateRequest(request, &error));
    }
};
QTEST_GUILESS_MAIN(PrefixTest)
#include "prefix_test.moc"
