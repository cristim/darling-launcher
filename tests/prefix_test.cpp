// SPDX-License-Identifier: GPL-3.0-or-later
#include "prefixbuilder.h"
#include "discovery.h"
#include "releases.h"
#include "core.h"
#include "release_fixture.h"
#include "log.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

namespace {
QJsonObject readJson(const QString &path) { QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return QJsonDocument::fromJson(file.readAll()).object(); }
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
        QVERIFY(QDir().mkpath(root + "/good/usr/local/bin")); QVERIFY(QDir().mkpath(root + "/evil"));
        { QFile f(root + "/good/usr/local/bin/darling"); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("x"); }
        QVERIFY(QFile::link("/usr/lib/libz.so", root + "/good/usr/local/libz"));
        run({"tar", "--zstd", "-cf", "good.tar.zst", "-C", "good", "usr"});
        QVERIFY(QFile::link("/etc", root + "/evil/dir")); { QFile f(root + "/evil/payload"); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("x"); }
        run({"tar", "--zstd", "-cf", "through.tar.zst", "-C", "evil", "dir", "--transform", "s,^payload,dir/payload,", "payload"});
        run({"tar", "--zstd", "-cf", "escape.tar.zst", "-C", "evil", "--transform", "s,^payload,../payload,", "payload"});
        const QString link = "lrwxrwxrwx u/g 0 2026-10-09 14:32 dir -> /etc", file = "-rw-r--r-- u/g 1 2026-10-09 14:32 ";
        for (const char *path : {"dir/payload", "./dir/./payload", "dir//payload", "dir/sub/../payload"}) QVERIFY2(!vetMembers({link, file + path}).isEmpty(), path);
        QVERIFY(vetMembers({link, file + "other/payload"}).isEmpty());
        QVERIFY(!vetMembers({"lrwxrwxrwx u/g 0 2026-10-09 14:32 a -> b -> /home/u/.ssh", file + "a -> b/authorized_keys"}).isEmpty());
        QVERIFY(!vetMembers({file + "a -> b"}).isEmpty());
        QVERIFY(!vetMembers({file + "a\\nb"}).isEmpty());
        auto script = [&](const QString &name, const QString &body) { QFile f(root + "/" + name); f.open(QIODevice::WriteOnly); f.write(("#!/bin/sh\n" + body + "\n").toUtf8()); f.close(); f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner); return f.fileName(); };
        const QString ghOk = script("gh-ok", "echo \"$@\" > '" + root + "/gh-args'"), ghBad = script("gh-bad", "echo untrusted >&2; exit 1"), repo = "VibeDarling/darling";
        const QString tag = "v2026.10.09-3721b65", good = root + "/good.tar.zst", runtimes = root + "/runtimes";
        QVERIFY(QDir().mkpath(root + "/linked/usr/local/bin")); QVERIFY(QFile::link("/usr/local/bin/darling", root + "/linked/usr/local/bin/darling"));
        run({"tar", "--zstd", "-cf", "linked.tar.zst", "-C", "linked", "usr"});
        const QString linked = root + "/linked.tar.zst";
        QVERIFY(installArchive(linked, digest(linked), runtimes, "v2026.10.09-aaaaaaa", ghOk, repo).contains("non-symlink"));
        QVERIFY(installArchive(good, digest(good), runtimes, "latest", ghOk, repo).contains("not a release tag"));
        QVERIFY2(installArchive(good, QString(64, '0'), runtimes, tag, ghOk, repo).contains("SHA-256 mismatch"), "digest");
        QVERIFY(installArchive(good, digest(good), runtimes, tag, ghBad, repo).contains("attestation verification failed"));
        QVERIFY(installArchive(good, digest(good), runtimes, tag, {}, repo).contains("gh is required"));
        QVERIFY(!QFileInfo::exists(runtimes + "/" + tag));
        QVERIFY(!QFileInfo::exists(runtimes + "/" + tag));
        for (const char *name : {"through", "escape"}) { const QString archive = root + "/" + name + ".tar.zst"; const QString error = installArchive(archive, digest(archive), runtimes, tag, ghOk, repo); QVERIFY2(!error.isEmpty() && (error.contains("symlink") || error.contains("escapes")), name); QVERIFY(!QFileInfo::exists(runtimes + "/" + tag)); }
        QCOMPARE(installArchive(good, digest(good), runtimes, tag, ghOk, repo), QString());
        QVERIFY(QFileInfo(runtimes + "/" + tag + "/usr/local/bin/darling").isFile());
        QVERIFY(QFile(root + "/gh-args").open(QIODevice::ReadOnly)); { QFile args(root + "/gh-args"); args.open(QIODevice::ReadOnly); const QByteArray text = args.readAll(); QVERIFY(text.contains("--signer-workflow VibeDarling/darling/.github/workflows/release-binaries.yml")); QVERIFY(text.contains("--source-ref refs/heads/master")); QVERIFY(text.contains("--deny-self-hosted-runners")); }
        QVERIFY(QFileInfo(runtimes + "/" + tag + "/usr/local/libz").isSymLink());
        QVERIFY(installArchive(good, digest(good), runtimes, tag, ghOk, repo).contains("already exists"));
    }
    void latestReleaseIsDownloadedAndInstalled() {
        using namespace LauncherReleases;
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); const QString root = temporary.path(), runtimes = root + "/runtimes";
        FakeRelease release; QVERIFY(release.create(root));
        const QString &gh = release.gh, &repo = release.repo, &tag = release.tag, &file = release.file, &sha = release.sha;
        auto leftovers = [&] { return QDir(runtimes).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).filter(".download"); };

        QVERIFY(!installLatest({}, repo, runtimes).isEmpty());
        for (const char *bad : {"not a repo; rm -rf", "../..", "a/..", "a/b/c"}) QVERIFY2(installLatest(gh, bad, runtimes).contains("not a GitHub repository"), bad);
        QVERIFY(release.calls().isEmpty());
        QVERIFY(release.publish(sha, false));
        QVERIFY2(installLatest(gh, repo, runtimes).contains("No prebuilt runtime release"), "only changelog, prerelease and draft releases");
        QVERIFY(!release.calls().contains("release download"));
        QVERIFY(release.publish(QString(64, '0')));
        QVERIFY(installLatest(gh, repo, runtimes).contains("SHA-256 mismatch"));
        QVERIFY(!QFileInfo::exists(runtimes + "/" + tag)); QVERIFY(leftovers().isEmpty());
        QVERIFY(release.publish(sha)); QVERIFY(QFile::remove(release.fake + "/assets/manifest.json"));
        QVERIFY(installLatest(gh, repo, runtimes).contains("gh release download failed"));
        QVERIFY(leftovers().isEmpty());
        QVERIFY(release.publish(sha, true, QJsonArray{"libdefinitely-missing.so.9"})); release.clearCalls();
        QVERIFY(installLatest(gh, repo, runtimes).contains("libdefinitely-missing.so.9"));
        QVERIFY2(!release.calls().contains("--pattern " + file), "a missing host library must stop before the archive download");
        QVERIFY(leftovers().isEmpty());
        QVERIFY(release.publish(sha, true, {}, "99.1")); release.clearCalls();
        QVERIFY(installLatest(gh, repo, runtimes).contains("needs 99.1"));
        QVERIFY2(!release.calls().contains("--pattern " + file), "a too-old glibc must stop before the archive download");
        QVERIFY(release.publish(sha));
        QVERIFY(QDir().mkpath(runtimes + "/" + tag + ".partial")); release.clearCalls();
        QString installed;
        QVERIFY(installLatest(gh, repo, runtimes, &installed).contains("left over")); QVERIFY(installed.isEmpty());
        QVERIFY2(!release.calls().contains("release download"), "a leftover .partial must fail before any download");
        QVERIFY(QDir(runtimes + "/" + tag + ".partial").removeRecursively());
        QCOMPARE(installLatest(gh, repo, runtimes, &installed), QString());
        QCOMPARE(installed, tag);
        QVERIFY(QFileInfo(runtimes + "/" + tag + "/usr/local/bin/darling").isFile()); QVERIFY(leftovers().isEmpty());
        const QString log = release.calls();
        QVERIFY(log.contains("release download " + tag + " --repo " + repo + " --pattern manifest.json --dir " + runtimes + "/.download-"));
        QVERIFY(log.contains("release download " + tag + " --repo " + repo + " --pattern " + file + " --dir " + runtimes + "/.download-"));
        QVERIFY(log.contains("attestation verify"));
        release.clearCalls(); installed.clear();
        QCOMPARE(installLatest(gh, repo, runtimes, &installed), QString()); QCOMPARE(installed, tag);
        QVERIFY2(!release.calls().contains("release download"), "an installed tag must not be downloaded again");
        const auto setup = LauncherCore::setupPrebuilt(gh, repo, root);
        QVERIFY2(setup.error.isEmpty(), qPrintable(setup.error));
        QCOMPARE(setup.launcher, runtimes + "/" + tag + "/usr/local/bin/darling"); QCOMPARE(setup.runtimeRoot, runtimes + "/" + tag + "/usr/local");
        QCOMPARE(setup.prefix, root + "/prefixes/default"); QVERIFY(QFileInfo(setup.prefix + "/private/etc").isDir());
        QVERIFY(FakeRelease::write(root + "/prefixes/fail-uname", "x"));
        const auto broken = LauncherCore::setupPrebuilt(gh, repo, root);
        QVERIFY2(broken.error.contains("failed its checks") && broken.error.contains("System reports Darwin"), qPrintable(broken.error));
        QVERIFY(QFileInfo(runtimes + "/" + tag + "/usr/local/bin/darling").isFile()); release.clearCalls();
        QVERIFY(QFile::remove(root + "/prefixes/fail-uname")); QVERIFY(FakeRelease::write(root + "/prefixes/fail-ls", "x"));
        QVERIFY(LauncherCore::setupPrebuilt(gh, repo, root).error.contains("Library present: /usr/lib/libSystem.B.dylib"));
        QVERIFY(QFile::remove(root + "/prefixes/fail-ls")); QVERIFY(FakeRelease::write(root + "/prefixes/fail-init", "x"));
        const auto noInit = LauncherCore::setupPrebuilt(gh, repo, root);
        QVERIFY2(noInit.error.contains("Initialize the prefix: init-broken") && !noInit.error.contains("System reports Darwin"), qPrintable(noInit.error));
        QVERIFY(QFile::remove(root + "/prefixes/fail-init"));
        QVERIFY2(LauncherCore::setupPrebuilt(gh, repo, root).error.isEmpty(), "a retry after fixing the prefix succeeds");
        QVERIFY2(!release.calls().contains("release download"), "retries reuse the installed runtime");
        QVERIFY(LauncherCore::setupPrebuilt({}, repo, root + "/other").error.contains("gh is required"));
        const auto rel = [&](const QString &name, const QString &created, bool manifest) { return QJsonObject{{"tag_name", name}, {"created_at", created}, {"assets", manifest ? QJsonArray{release.asset("manifest.json")} : QJsonArray{}}}; };
        QCOMPARE(latestRuntimeRelease(QJsonArray{rel("v2026.10.01-aaaaaaa", "2026-10-01T00:00:00Z", true), rel("v2026.10.05-bbbbbbb", "2026-10-05T00:00:00Z", true), rel("v2026.10.07-ccccccc", "2026-10-07T00:00:00Z", false)}).value("tag_name").toString(), QString("v2026.10.05-bbbbbbb"));
        QCOMPARE(latestRuntimeRelease(QJsonArray{rel("v2026.10.05-bbbbbbb", "2026-10-05T00:00:00Z", true), rel("v2026.10.01-aaaaaaa", "2026-10-01T00:00:00Z", true)}).value("tag_name").toString(), QString("v2026.10.05-bbbbbbb"));
        QVERIFY(latestRuntimeRelease(QJsonArray{}).isEmpty());
        QVERIFY(QDir(runtimes + "/" + tag).removeRecursively()); QVERIFY(QFile::link(root + "/image", runtimes + "/" + tag));
        QVERIFY(installLatest(gh, repo, runtimes).contains("not a runtime folder"));
    }
    void hostPreflight() {
        using namespace LauncherReleases;
        Artifact artifact; artifact.glibc = "2.39"; artifact.wrappedSonames = {"libX11.so.6", "libz.so.1"};
        const QStringList host{"libX11.so.6", "libz.so.1", "libc.so.6"};
        QVERIFY(preflight(artifact, "2.39", host).isEmpty());
        QVERIFY(preflight(artifact, "2.41", host).isEmpty());
        QVERIFY(preflight(artifact, "2.4", host).contains("glibc 2.4")); // 2.4 is older than 2.39, not newer
        QVERIFY(preflight(artifact, "2.38", host).contains("needs 2.39"));
        QVERIFY(preflight(artifact, "2.39", {"libz.so.1"}).contains("libX11.so.6"));
        QVERIFY(!preflight(artifact, "2.39", {"libz.so.1"}).contains("libz.so.1"));
        artifact.glibc.clear(); QVERIFY(preflight(artifact, "1.0", host).isEmpty());
        const QStringList listing{"3117 libs found in cache `/etc/ld.so.cache'", "\tlibX11.so.6 (libc6,AArch64) => /usr/lib/libX11.so.6", "\tlibX11.so.6 (libc6,x86-64) => /usr/lib/x86_64/libX11.so.6", "\tlibabi.so.1 (libc6,x86-64, OS ABI: Linux 3.2.0) => /usr/lib/libabi.so.1", "\tlibold.so.1 (libc6) => /usr/lib32/libold.so.1", "garbage"};
        QCOMPARE(parseLdconfig(listing, "aarch64"), QStringList{"libX11.so.6"});
        QCOMPARE(parseLdconfig(listing, "x86_64"), (QStringList{"libX11.so.6", "libabi.so.1"}));
        QVERIFY(parseLdconfig(listing, "riscv64").isEmpty());
        QString error; const QStringList real = hostSonames(hostArchitecture(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error)); QVERIFY(real.contains(QStringLiteral("libc.so.6")));
    }
    void releaseTagsAndSelection() {
        using namespace LauncherReleases;
        for (const char *good : {"v2026.10.09-3721b65", "v2026.10.09-3721b65-r2"}) QVERIFY2(validTag(good), good);
        for (const char *bad : {"2026-10-09-14-32-3721b65", "v2026.10.09-3721b6", "v2026.10.09-3721b65.partial", "v2026.10.09-XYZ1234", "latest", "../v2026.10.09-3721b65"}) QVERIFY2(!validTag(bad), bad);
        QVERIFY(!hostArchitecture().isEmpty());
        const QString tag = "v2026.10.09-3721b65", file = "darling-runtime-" + tag + "-linux-aarch64.tar.zst", url = "https://github.com/VibeDarling/darling/releases/download/" + tag + "/" + file;
        const QJsonObject entry{{"file", file}, {"url", url}, {"sha256", QString(64, 'a')}, {"size", 1}, {"unpacked_size", 2}, {"install_root", "usr/local"}, {"launcher", "usr/local/bin/darling"}, {"host_requires", QJsonObject{{"glibc", "2.39"}, {"wrapped_sonames", QJsonArray{"libX11.so.6", "libz.so.1"}}}}};
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
        QCOMPARE(chosen.artifact.glibc, QString("2.39")); QCOMPARE(chosen.artifact.wrappedSonames, (QStringList{"libX11.so.6", "libz.so.1"}));
        QVERIFY(select(release, entryWith("host_requires", QJsonValue::Null), "aarch64", false).error.contains("no host_requires"));
        QVERIFY(select(release, entryWith("host_requires", QJsonObject{{"glibc", QJsonValue::Null}, {"wrapped_sonames", QJsonArray{}}}), "aarch64", false).valid());
        QVERIFY(select(release, entryWith("host_requires", QJsonObject{{"glibc", "2.x"}, {"wrapped_sonames", QJsonArray{}}}), "aarch64", false).error.contains("not a version"));
        QVERIFY(select(release, entryWith("host_requires", QJsonObject{{"glibc", "2.39"}}), "aarch64", false).error.contains("wrapped_sonames"));
        QVERIFY(select(release, entryWith("host_requires", QJsonObject{{"glibc", "2.39"}, {"wrapped_sonames", QJsonArray{"../../etc/passwd"}}}), "aarch64", false).error.contains("invalid library name"));
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
