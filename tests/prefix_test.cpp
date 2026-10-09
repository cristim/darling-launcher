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
        const QString tag = "2026-10-09-14-32-3721b65", good = root + "/good.tar.zst", runtimes = root + "/runtimes";
        QVERIFY(installArchive(good, digest(good), runtimes, "latest").contains("not a release tag"));
        QVERIFY2(installArchive(good, QString(64, '0'), runtimes, tag).contains("SHA-256 mismatch"), "digest");
        QVERIFY(!QFileInfo::exists(runtimes + "/" + tag));
        for (const char *name : {"through", "escape"}) { const QString archive = root + "/" + name + ".tar.zst"; const QString error = installArchive(archive, digest(archive), runtimes, tag); QVERIFY2(!error.isEmpty() && (error.contains("symlink") || error.contains("escapes")), name); QVERIFY(!QFileInfo::exists(runtimes + "/" + tag)); }
        QCOMPARE(installArchive(good, digest(good), runtimes, tag), QString());
        QVERIFY(QFileInfo(runtimes + "/" + tag + "/usr/local/bin/darling").isFile());
        QVERIFY(QFileInfo(runtimes + "/" + tag + "/usr/local/libz").isSymLink());
        QVERIFY(installArchive(good, digest(good), runtimes, tag).contains("already exists"));
    }
    void releaseTagsAndSelection() {
        using namespace LauncherReleases;
        QVERIFY(parseTag("2026-10-09-14-32-3721b65")); QCOMPARE(parseTag("2026-10-09-14-32-3721b65")->time, QDateTime(QDate(2026, 10, 9), QTime(14, 32), Qt::UTC));
        for (const char *bad : {"v2026.10.09-3721b65", "2026-10-09-14-32", "2026-13-09-14-32-3721b65", "2026-10-09-25-32-3721b65", "2026-10-09-14-32-XYZ", "2026-10-09T14:32-3721b65"}) QVERIFY2(!parseTag(bad), bad);
        auto release = [](const QString &tag, const QString &published, bool draft, const QStringList &assets) {
            QJsonArray list; for (const auto &name : assets) list.append(QJsonObject{{"name", name}, {"browser_download_url", "https://example.invalid/" + tag + "/" + name}, {"digest", "sha256:" + QString(64, 'a')}});
            return QJsonObject{{"tag_name", tag}, {"published_at", published}, {"draft", draft}, {"assets", list}};
        };
        const QString arm = assetName("runtime", "arm64");
        QCOMPARE(arm, QString("darling-runtime-arm64.tar.zst"));
        QJsonArray list{release("2026-10-08-10-00-aaaaaaa", "2026-10-08T10:01:00Z", false, {arm}), release("2026-10-09-14-32-3721b65", "2026-10-09T14:35:00Z", false, {arm, assetName("runtime", "x86_64")}),
                        release("2026-10-10-09-00-bbbbbbb", "2026-10-10T09:01:00Z", true, {arm}), release("garbage", "2026-10-11T09:01:00Z", false, {arm})};
        auto chosen = latest(list, "runtime", "arm64"); QVERIFY2(chosen.valid(), qPrintable(chosen.error));
        QCOMPARE(chosen.release.tag, QString("2026-10-09-14-32-3721b65")); QCOMPARE(chosen.asset.sha256, QString(64, 'a')); QVERIFY(chosen.asset.url.endsWith(arm));
        QVERIFY(latest(list, "runtime", "x86_64").valid());
        QVERIFY(latest(list, "runtime", "riscv64").error.contains("has no darling-runtime-riscv64.tar.zst"));
        QVERIFY(latest(QJsonArray{release("2026-10-09-14-32-3721b65", "2026-10-09T14:35:00Z", false, {})}, "runtime", "arm64").error.contains("has no"));
        QVERIFY(latest({}, "runtime", "arm64").error.contains("No usable release"));
        auto noDigest = release("2026-10-09-14-32-3721b65", "2026-10-09T14:35:00Z", false, {arm}); auto assets = noDigest.value("assets").toArray(); auto first = assets.first().toObject(); first.remove("digest"); assets.replace(0, first); noDigest.insert("assets", assets);
        QVERIFY(latest(QJsonArray{noDigest}, "runtime", "arm64").error.contains("SHA-256"));
        QVERIFY(latest(QJsonArray{release("2026-10-09-14-32-aaaaaaa", "2026-10-09T14:35:00Z", false, {arm}), release("2026-10-09-14-32-bbbbbbb", "2026-10-09T14:35:00Z", false, {arm})}, "runtime", "arm64").error.contains("cannot be ordered"));
        QCOMPARE(latest(QJsonArray{release("2026-10-09-14-32-aaaaaaa", "2026-10-09T14:35:00Z", false, {arm}), release("2026-10-09-14-32-bbbbbbb", "2026-10-09T14:40:00Z", false, {arm})}, "runtime", "arm64").release.sha, QString("bbbbbbb"));
    }
    void prebuiltRuntimesAreDiscoveredByTag() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); qputenv("HOME", temporary.path().toUtf8());
        const QString tag = LauncherDiscovery::dataRoot() + "/runtimes/2026-10-09-14-32-3721b65";
        QVERIFY(QDir().mkpath(tag + "/usr/local/bin")); QVERIFY(QDir().mkpath(tag + "/usr/local/libexec/darling/private/etc"));
        QFile launcher(tag + "/usr/local/bin/darling"); QVERIFY(launcher.open(QIODevice::WriteOnly)); launcher.close(); QVERIFY(launcher.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto found = LauncherDiscovery::runtimes({}, {}); QCOMPARE(found.size(), 1);
        QCOMPARE(found.first().launcher, launcher.fileName()); QCOMPARE(found.first().installRoot, tag + "/usr/local");
        QVERIFY(QFile::remove(launcher.fileName())); QVERIFY(LauncherDiscovery::runtimes({}, {}).isEmpty());
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
