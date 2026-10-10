// SPDX-License-Identifier: GPL-3.0-or-later
#include "prefixbuilder.h"
#include "discovery.h"
#include "log.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

namespace {
QJsonObject readJson(const QString &path) { QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return QJsonDocument::fromJson(file.readAll()).object(); }
}
static QString sha256(const QString &path) {
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex());
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
    void heavyBuildLockIsPrivatePerUser() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        const QByteArray previousOverride = qgetenv("DARLING_LAUNCHER_LOCK_DIR"), previousRuntime = qgetenv("XDG_RUNTIME_DIR");
        auto restore = qScopeGuard([=] { qputenv("DARLING_LAUNCHER_LOCK_DIR", previousOverride); qputenv("XDG_RUNTIME_DIR", previousRuntime); });
        qunsetenv("DARLING_LAUNCHER_LOCK_DIR"); qputenv("XDG_RUNTIME_DIR", temporary.path().toUtf8());
        QString error; const QString directory = temporary.path() + "/darling-launcher";
        QCOMPARE(LauncherPrefix::heavyBuildLock(&error), directory + "/darling-heavy-build.lock");
        QCOMPARE(QFileInfo(directory).permissions() & (QFile::WriteGroup | QFile::WriteOther | QFile::ReadOther | QFile::ExeOther), QFile::Permissions());
        // A directory another user could have planted entries in, or a symlink to one, is refused.
        QVERIFY(QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::WriteOther | QFile::ExeOther | QFile::ReadOther));
        QVERIFY(LauncherPrefix::heavyBuildLock(&error).isEmpty()); QVERIFY(error.contains(directory));
        QVERIFY(QDir(directory).removeRecursively()); QVERIFY(QDir().mkpath(temporary.path() + "/elsewhere")); QVERIFY(QFile::link(temporary.path() + "/elsewhere", directory));
        QVERIFY(LauncherPrefix::heavyBuildLock(&error).isEmpty());
        // The override is honoured as is, under the same ownership rules.
        qputenv("DARLING_LAUNCHER_LOCK_DIR", (temporary.path() + "/override").toUtf8());
        QCOMPARE(LauncherPrefix::heavyBuildLock(&error), temporary.path() + "/override/darling-heavy-build.lock");
        qputenv("DARLING_LAUNCHER_LOCK_DIR", "relative/locks"); QVERIFY(LauncherPrefix::heavyBuildLock(&error).isEmpty());
        qunsetenv("DARLING_LAUNCHER_LOCK_DIR"); qunsetenv("XDG_RUNTIME_DIR"); QVERIFY(LauncherPrefix::heavyBuildLock(&error).isEmpty()); QVERIFY(error.contains("XDG_RUNTIME_DIR"));
    }
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
        QVERIFY(LauncherPrefix::selectInputs(discovery, false, &selected, &error));
        QVERIFY(selected.value("repos").toArray().first().toObject().value("prs").toArray().isEmpty());
        QCOMPARE(discovery.value("repos").toArray().first().toObject().value("prs").toArray().size(), 1);
        QVERIFY(LauncherPrefix::selectInputs(discovery, true, &selected, &error));
        QCOMPARE(selected.value("repos"), discovery.value("repos"));
        discovery.insert("complete", false); QVERIFY(!LauncherPrefix::selectInputs(discovery, false, &selected, &error));
    }
    void externalBuilder_data() {
        QTest::addColumn<bool>("includePrs");
        QTest::addColumn<bool>("nested");
        QTest::newRow("default-branches-legacy") << false << false; QTest::newRow("include-prs-legacy") << true << false;
        QTest::newRow("default-branches-nested") << false << true; QTest::newRow("include-prs-nested") << true << true;
    }
    void externalBuilder() {
        QFETCH(bool, includePrs); QFETCH(bool, nested);
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QString source = temporary.path() + "/source", workspace = temporary.path() + "/new workspace";
        QVERIFY(QDir().mkpath(source));
        QString script = QString(TEST_SOURCE_DIR) + "/tests/fixtures/prefix_builder.py";
        if (!nested) {
            QFile input(script); QVERIFY(input.open(QIODevice::ReadOnly)); auto bytes = input.readAll(); bytes.replace(", \"resolve-nested\", \"checkout-nested\"", "");
            script = temporary.path() + "/legacy.py"; QFile legacy(script); QVERIFY(legacy.open(QIODevice::WriteOnly)); legacy.write(bytes); legacy.close();
        }
        PrefixBuildRequest request{QStandardPaths::findExecutable("python3"), script, source, workspace, includePrs, 2, {"-DENABLE_TESTS=ON"}, sha256(script)};
        PrefixBuilder builder; QSignalSpy completed(&builder, &PrefixBuilder::completed), phases(&builder, &PrefixBuilder::phaseChanged);
        builder.start(request); QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 5000);
        QVERIFY2(completed.first()[0].toBool(), qPrintable(completed.first()[1].toString())); QCOMPARE(phases.size(), nested ? 6 : 4);
        QCOMPARE(completed.first()[2].toString(), workspace + "/prefix");
        auto discovery = readJson(workspace + "/refs.discovery.lock.json"), selected = readJson(workspace + "/refs.lock.json");
        QCOMPARE(discovery.value("repos").toArray().first().toObject().value("prs").toArray().size(), includePrs ? 1 : 0);
        QCOMPARE(selected.value("repos").toArray().first().toObject().value("prs").toArray().size(), includePrs ? 1 : 0);
        auto provenance = readJson(workspace + "/prefix/.darling-launcher/build-provenance.json");
        QCOMPARE(provenance.value("script_sha256").toString().size(), 64);
        QCOMPARE(provenance.value("include_open_prs").toBool(), includePrs);
        if (nested) {
            auto lock = readJson(workspace + "/nested.refs.lock.json"), original = readJson(workspace + "/nested.discovery.lock.json");
            QCOMPARE(lock.value("vibedarling").toArray().first().toObject().value("prs").toArray().size(), includePrs ? 1 : 0);
            QCOMPARE(original.value("vibedarling").toArray().first().toObject().value("prs").toArray().size(), includePrs ? 1 : 0);
        }
        QCOMPARE(readJson(workspace + "/build-call.json").value("cmake_args").toArray(), QJsonArray{"-DENABLE_TESTS=ON"});
        QVERIFY(QDir(source).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    }
    void builderRunsOnlyThePinnedScript() {
        // The script is chosen (and hashed) before the build starts; a change in between must not run.
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QString source = temporary.path() + "/source"; QVERIFY(QDir().mkpath(source));
        const QString script = temporary.path() + "/builder.py";
        QVERIFY(QFile::copy(QString(TEST_SOURCE_DIR) + "/tests/fixtures/prefix_builder.py", script));
        const QString pinned = sha256(script);
        QFile original(script); QVERIFY(original.open(QIODevice::ReadOnly)); const QByteArray body = original.readAll(); original.close();
        QFile changed(script); QVERIFY(changed.open(QIODevice::WriteOnly | QIODevice::Truncate));
        changed.write("import pathlib; pathlib.Path(" + QByteArray("'") + QFile::encodeName(temporary.path() + "/ran") + "').write_text('x')\n" + body); changed.close();
        PrefixBuilder builder; QSignalSpy completed(&builder, &PrefixBuilder::completed), phases(&builder, &PrefixBuilder::phaseChanged);
        builder.start({QStandardPaths::findExecutable("python3"), script, source, temporary.path() + "/new", false, 1, {}, pinned});
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 5000); QVERIFY(!completed.first()[0].toBool());
        QVERIFY2(completed.first()[1].toString().contains("changed after it was selected"), qPrintable(completed.first()[1].toString()));
        QCOMPARE(phases.size(), 0); QVERIFY(!QFileInfo::exists(temporary.path() + "/ran"));
        PrefixBuildRequest unpinned{QStandardPaths::findExecutable("python3"), script, source, temporary.path() + "/new", false, 1, {}}; QString error;
        QVERIFY(!LauncherPrefix::validateRequest(unpinned, &error));
    }
    void stopOnIntegrationFailure() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QString source = temporary.path() + "/source"; QVERIFY(QDir().mkpath(source));
        QFile marker(source + "/fail-checkout"); QVERIFY(marker.open(QIODevice::WriteOnly)); marker.close();
        PrefixBuilder builder; QSignalSpy completed(&builder, &PrefixBuilder::completed), phases(&builder, &PrefixBuilder::phaseChanged);
        const QString fixture = QString(TEST_SOURCE_DIR) + "/tests/fixtures/prefix_builder.py";
        builder.start({QStandardPaths::findExecutable("python3"), fixture, source, temporary.path() + "/new", true, 1, {}, sha256(fixture)});
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 5000); QVERIFY(!completed.first()[0].toBool()); QCOMPARE(phases.size(), 3);
        QVERIFY(!QFileInfo::exists(temporary.path() + "/new/build-call.json"));
    }
    void refuseExistingOrSharedWorkspace() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QString source = temporary.path() + "/source"; QVERIFY(QDir().mkpath(source));
        const QString fixture = QString(TEST_SOURCE_DIR) + "/tests/fixtures/prefix_builder.py";
        PrefixBuildRequest request{QStandardPaths::findExecutable("python3"), fixture, source, source, false, 1, {}, sha256(fixture)}; QString error;
        QVERIFY(!LauncherPrefix::validateRequest(request, &error));
        request.workspace = source + "/new"; QVERIFY(!LauncherPrefix::validateRequest(request, &error));
        request.workspace = temporary.path() + "/new"; request.cmakeArguments = {"shell-command"}; QVERIFY(!LauncherPrefix::validateRequest(request, &error));
    }
};
QTEST_GUILESS_MAIN(PrefixTest)
#include "prefix_test.moc"
