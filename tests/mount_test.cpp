// SPDX-License-Identifier: GPL-3.0-or-later
#include "mount.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <unistd.h>

class MountTest : public QObject {
    Q_OBJECT
private slots:
    void partitionDiscovery() {
        QString error;
        auto partitions = LauncherMount::parsePartitions(R"({"blockdevices":[{"path":"/dev/disk","fstype":null,"children":[{"path":"/dev/disk1","fstype":"apfs","mountpoints":[null]},{"path":"/dev/disk2","fstype":"hfsplus","label":"macOS","mountpoints":["/mnt/mac"]},{"path":"/dev/disk3","fstype":"ext4","mountpoints":[]}]}]})", &error);
        QVERIFY(error.isEmpty()); QCOMPARE(partitions.size(), 2);
        QCOMPARE(partitions[0].device, "/dev/disk1"); QVERIFY(!partitions[0].mounted);
        QCOMPARE(partitions[1].label, "macOS"); QVERIFY(partitions[1].mounted); QCOMPARE(partitions[1].mountPoints, QStringList{"/mnt/mac"});
        QVERIFY(LauncherMount::parsePartitions("bad JSON", &error).isEmpty()); QVERIFY(!error.isEmpty());
    }
    // Anyone can attach a loop device with an image of their choosing, which root would then parse.
    void loopDevicesAreNotOffered() {
        QString error;
        const auto partitions = LauncherMount::parsePartitions(R"({"blockdevices":[{"path":"/dev/loop0","type":"loop","fstype":"apfs"},{"path":"/dev/loop1","type":"loop","fstype":null,"children":[{"path":"/dev/loop1p1","type":"part","fstype":"hfsplus"}]},{"path":"/dev/sda","type":"disk","fstype":null,"children":[{"path":"/dev/sda2","type":"part","fstype":"apfs"}]}]})", &error);
        QVERIFY(error.isEmpty()); QCOMPARE(partitions.size(), 1); QCOMPARE(partitions[0].device, "/dev/sda2");
    }
    // Every component of the resolved path must be root-owned (or extraOwner) and not group/other writable.
    void writeBitsOnAnyComponentRefuseTheHelper() {
        QTemporaryDir temporary(QDir::homePath() + "/launcher-mount-test-XXXXXX"); QVERIFY(temporary.isValid());
        const unsigned me = getuid(); QString error;
        const QString dir = temporary.path() + "/libexec", tool = dir + "/helper"; QVERIFY(QDir().mkpath(dir));
        QFile file(tool); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("#!/bin/sh\n"); file.close();
        const auto mode = [](const QString &path, QFile::Permissions extra) { return QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup | QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther | extra); };
        QVERIFY(mode(tool, {})); QVERIFY(mode(dir, {}));
        if (LauncherMount::trustedExecutable(tool, me, &error).isEmpty()) QSKIP(qPrintable("home directory is not usable as a trusted tree: " + error));
        for (const auto bit : {QFile::WriteGroup, QFile::WriteOther}) {
            QVERIFY(mode(tool, bit)); QVERIFY2(LauncherMount::trustedExecutable(tool, me, &error).isEmpty(), "file"); QVERIFY(mode(tool, {}));
            QVERIFY(mode(dir, bit)); QVERIFY2(LauncherMount::trustedExecutable(tool, me, &error).isEmpty(), "directory"); QVERIFY(mode(dir, {}));
        }
        QVERIFY(!LauncherMount::trustedExecutable(tool, me, &error).isEmpty());
        QVERIFY(LauncherMount::trustedExecutable(tool, 0, &error).isEmpty());  // owned by this user, not root
    }
    // Root unmounts only a direct child of a parent only root (here: the test user) can change.
    void helperMountNeedsADirectChildOfATrustedParent() {
        QTemporaryDir temporary(QDir::homePath() + "/launcher-mount-test-XXXXXX"); QVERIFY(temporary.isValid());
        const unsigned me = getuid(); const QString parent = temporary.path() + "/parent", child = parent + "/sda1-volume-0";
        QVERIFY(QDir().mkpath(child)); QVERIFY(QDir().mkpath(parent + "/other/deep"));
        QVERIFY(QFile::setPermissions(temporary.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(QFile::setPermissions(parent, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        if (!LauncherMount::isHelperMount(child, parent, me)) QSKIP("home directory is not usable as a trusted tree");
        QVERIFY(!LauncherMount::isHelperMount(parent + "/other/deep", parent, me));  // not a direct child
        QVERIFY(!LauncherMount::isHelperMount(parent, parent, me));
        QVERIFY(QFile::link(child, parent + "/link")); QVERIFY(!LauncherMount::isHelperMount(parent + "/link", parent, me));
        QVERIFY(!LauncherMount::isHelperMount(child, parent, 0));  // the parent chain is owned by this user
        for (const auto bit : {QFile::WriteGroup, QFile::WriteOther}) {
            QVERIFY(QFile::setPermissions(parent, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | bit));
            QVERIFY(!LauncherMount::isHelperMount(child, parent, me));
        }
    }
    void onlyRootOwnedHelpersReachPkexec() {
        // A user-writable helper (build output, PATH entry) must never be run as root.
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); QString error;
        QString fake = temporary.path() + "/apfs-fuse"; QFile file(fake); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("#!/bin/sh\nexit 0\n"); file.close();
        QVERIFY(QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadOther | QFile::ExeOther));
        QVERIFY(LauncherMount::trustedExecutable(fake, 0, &error).isEmpty()); QVERIFY2(error.contains("Refusing to run"), qPrintable(error));
        QVERIFY(LauncherMount::trustedExecutable("usr/bin/true", 0, &error).isEmpty());
        QVERIFY(LauncherMount::trustedExecutable("/etc/passwd", 0, &error).isEmpty());
        // A user-owned symlink to a root-owned tool is resolved once and the resolved path is what runs.
        QString link = temporary.path() + "/link-to-true"; QVERIFY(QFile::link("/usr/bin/true", link));
        QCOMPARE(LauncherMount::trustedExecutable(link, 0, &error), QFileInfo("/usr/bin/true").canonicalFilePath());
    }
    void unmountOnlyHelperMounts() {
        // Root unmounts by path, so only paths nobody but root can re-point qualify.
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QCOMPARE(LauncherMount::mountParent(), "/run/darling-launcher/" + QString::number(getuid()));
        QVERIFY(!LauncherMount::isHelperMount(temporary.path()));
        QVERIFY(!LauncherMount::isHelperMount(LauncherMount::mountParent() + "/../x"));
        QVERIFY(!LauncherMount::isHelperMount(LauncherMount::mountParent() + "/missing-volume-0"));
        QVERIFY(!LauncherMount::isHelperMount("relative/path"));
        QVERIFY(!LauncherMount::isHelperMount("/run"));
    }
    void cryptexCommandIsOneArgvList() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QString script = temporary.path() + "/mount-cryptex.py"; QFile file(script); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        const QString tool = "/usr/bin/true"; const QString root = "/run/user/1000/mount root";
        MountCommand command; QString error;
        QVERIFY2(LauncherMount::makeCryptexCommand(tool, script, root, tool, {}, &command, &error), qPrintable(error));
        QCOMPARE(command.program, tool);
        QCOMPARE(command.arguments, (QStringList{"-I", script, "--mount-root", root, "--apfs-fuse", tool}));
        QVERIFY(LauncherMount::makeCryptexCommand(tool, script, root, tool, "/dev/nvme0n1p3", &command, &error));
        QCOMPARE(command.arguments.mid(command.arguments.size() - 2), (QStringList{"--device", "/dev/nvme0n1p3"}));
    }
    void cryptexCommandRefusals() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QString script = temporary.path() + "/mount-cryptex.py"; QFile file(script); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        const QString tool = "/usr/bin/true", root = "/run/user/1000/mounts"; MountCommand command; QString error;
        for (const QString &badRoot : {QString(), QString("relative"), QString("/"), QString("/run/../etc")})
            QVERIFY(!LauncherMount::makeCryptexCommand(tool, script, badRoot, tool, {}, &command, &error));
        for (const QString &badDevice : {QString("/dev/sda2;touch x"), QString("sda2"), QString("/dev/../etc/passwd")})
            QVERIFY(!LauncherMount::makeCryptexCommand(tool, script, root, tool, badDevice, &command, &error));
        QVERIFY(!LauncherMount::makeCryptexCommand("", script, root, tool, {}, &command, &error));
        QVERIFY(!LauncherMount::makeCryptexCommand(tool, script, root, temporary.path() + "/missing", {}, &command, &error));
        QVERIFY(!LauncherMount::makeCryptexCommand(tool, script, root, "apfs-fuse", {}, &command, &error));
        QVERIFY(!LauncherMount::makeCryptexCommand(tool, temporary.path() + "/missing.py", root, tool, {}, &command, &error));
        QVERIFY(!error.isEmpty());
    }
    void homePrefixMatchesTheScriptOnSharedCases() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        const QString real = QFileInfo(temporary.path()).canonicalFilePath() + "/real", alias = temporary.path() + "/alias";
        QVERIFY(QDir().mkpath(real + "/me")); QVERIFY(QFile::link(real, alias));
        const QString dangling = temporary.path() + "/dangling"; QVERIFY(QFile::link(temporary.path() + "/gone", dangling));
        QFile file(QStringLiteral(TEST_SOURCE_DIR) + "/tests/home_prefix_cases.json"); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto cases = QJsonDocument::fromJson(file.readAll()).array(); QVERIFY(!cases.isEmpty());
        auto fill = [&](QString text) { return text.replace("@ALIAS@", alias).replace("@REAL@", real).replace("@DANGLING@", dangling); };
        for (const auto &value : cases) {
            const auto entry = value.toObject();
            // A null expectation means the script raises; the C++ side returns an empty string.
            const QString expected = entry["expect"].isNull() ? QString() : fill(entry["expect"].toString());
            QCOMPARE(LauncherMount::resolveHomePrefix(fill(entry["root"].toString()), fill(entry["home"].toString())), expected);
        }
    }
    void passwdHomeIsCleanAndIgnoresTheEnvironment() {
        const QByteArray saved = qgetenv("HOME"); qputenv("HOME", "/somewhere/else/");
        const QString home = LauncherMount::passwdHome();
        if (saved.isNull()) qunsetenv("HOME"); else qputenv("HOME", saved);
        QVERIFY(!home.isEmpty()); QVERIFY(QDir::isAbsolutePath(home)); QCOMPARE(home, QDir::cleanPath(home)); QVERIFY(home != "/somewhere/else");
    }
    void rootInterpreterIsTheOneConfigureChose() {
        const QString python = LauncherMount::launcherPython(); QString error;
        // CTest passes the configure-time choice (empty when none qualified): the baked constant must be that one.
        QVERIFY2(qEnvironmentVariableIsSet("LAUNCHER_EXPECTED_PYTHON"), "CTest passes the configure-time choice");
        QCOMPARE(python, qEnvironmentVariable("LAUNCHER_EXPECTED_PYTHON"));
        if (python.isEmpty()) QSKIP("No trusted Python was found when this build was configured");
        QVERIFY(QDir::isAbsolutePath(python));
        // The configure-time rule (cmake/SelectPython.cmake) and the runtime rule must agree on this path.
        QVERIFY2(!LauncherMount::trustedInterpreter(python, 0, &error).isEmpty(), qPrintable(error));
    }
    void withoutAnInterpreterTheRootActionsAreRefusedAndBuildNoCommand() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        const QString script = temporary.path() + "/mount-cryptex.py"; QFile file(script); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        MountCommand command{"untouched", {"untouched"}}; QString error;
        QVERIFY(LauncherMount::trustedInterpreter({}, 0, &error).isEmpty());
        QVERIFY(error.contains("No trusted Python 3.8+ was found when the launcher was configured")); QVERIFY(error.contains("README"));
        for (const QString &empty : {QString(), QString("")}) {
            error.clear();
            QVERIFY(!LauncherMount::makeCryptexCommand(empty, script, "/run/user/1000/mounts", "/usr/bin/true", {}, &command, &error));
            QCOMPARE(error, LauncherMount::noPythonMessage());
            error.clear();
            QVERIFY(!LauncherMount::makePrebootUnmountCommand(empty, script, &command, &error));
            QCOMPARE(error, LauncherMount::noPythonMessage());
        }
        QCOMPARE(command.program, QString("untouched")); QCOMPARE(command.arguments, QStringList{"untouched"});  // nothing built, so nothing can reach pkexec
        // a configured but untrusted path is refused too, with the ownership reason
        QVERIFY(LauncherMount::trustedInterpreter(script, 0, &error).isEmpty()); QVERIFY(!error.contains("No trusted Python"));
    }
    void rootRunFilesMustBeRootOwnedAndNotWritable() {
        // As root the test tree is root-owned and not writable by others, so the "untrusted" assertions cannot hold
        // (container CI); the same checks run for real when the suite runs as a normal user.
        if (::getuid() == 0) QSKIP("The ownership refusals cannot be shown when running as root");
        // Production accepts only root (extraOwner 0); the tests use a tree under HOME owned by the test user as the stand-in root.
        QTemporaryDir temporary(QDir::homePath() + "/.launcher-trust-XXXXXX"); QVERIFY(temporary.isValid());
        const QString directory = QFileInfo(temporary.path()).canonicalFilePath() + "/libexec", script = directory + "/mount-cryptex.py", helper = directory + "/mount-macos.py";
        QVERIFY(QDir().mkpath(directory));
        for (const QString &path : {script, helper}) { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.close(); }
        const auto exec = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup | QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther;
        QVERIFY(QFile::setPermissions(script, exec)); QVERIFY(QFile::setPermissions(helper, exec & ~QFile::ExeOwner & ~QFile::ExeGroup & ~QFile::ExeOther));
        const unsigned owner = ::getuid(); QString error;
        QVERIFY(LauncherMount::trustedFile(script, 0, true, &error).isEmpty()); QVERIFY(error.contains("not root-owned"));
        QVERIFY(!LauncherMount::trustedCryptexScript(script, 0, &error));
        QCOMPARE(LauncherMount::trustedFile(script, owner, true, &error), script);
        QVERIFY(LauncherMount::trustedCryptexScript(script, owner, &error));
        QVERIFY(LauncherMount::trustedFile(helper, owner, true, &error).isEmpty());
        QVERIFY(LauncherMount::trustedFile(directory + "/missing.py", owner, false, &error).isEmpty());
        QVERIFY(LauncherMount::trustedFile("relative.py", owner, false, &error).isEmpty());
        // group- or world-writable file, directory or ancestor
        QVERIFY(QFile::setPermissions(helper, exec | QFile::WriteGroup)); QVERIFY(!LauncherMount::trustedCryptexScript(script, owner, &error)); QVERIFY(error.contains("helper") || error.contains("mount-macos"));
        QVERIFY(QFile::setPermissions(helper, exec & ~QFile::ExeOwner & ~QFile::ExeGroup & ~QFile::ExeOther));
        QVERIFY(QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup));
        QVERIFY(!LauncherMount::trustedCryptexScript(script, owner, &error));
        QVERIFY(QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther));
        QVERIFY(LauncherMount::trustedFile(script, owner, true, &error).isEmpty());
        QVERIFY(QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup | QFile::ExeGroup));
        // a symlink is resolved and its target judged, not the link
        const QString link = temporary.path() + "/link.py"; QVERIFY(QFile::link(script, link));
        QCOMPARE(LauncherMount::trustedFile(link, owner, true, &error), script);
    }
    void unmountTargetsOnlyAVerifiedCryptexRoot() {
        // A failed second attempt (different root after a home-prefix symlink changed) must not replace the verified root.
        const QString first = "/home/u/mounts", second = "/var/home/u/mounts";
        QString verified = LauncherMount::adoptCryptexRoot({}, first, first + "/macos-cryptex-os");
        QCOMPARE(verified, first);
        QCOMPARE(LauncherMount::adoptCryptexRoot(verified, second, {}), first);                              // attempt failed: no event
        QCOMPARE(LauncherMount::adoptCryptexRoot(verified, second, first + "/macos-cryptex-os"), first);     // stale or foreign path
        QCOMPARE(LauncherMount::adoptCryptexRoot(verified, {}, second + "/macos-cryptex-os"), first);       // no attempt in progress
        QCOMPARE(LauncherMount::adoptCryptexRoot(verified, second, second + "/macos-cryptex-os"), second);  // second verified
        QCOMPARE(LauncherMount::adoptCryptexRoot({}, second, "/elsewhere/macos-cryptex-os"), QString());
    }
    void prebootUnmountIsOnlyTheScriptsUnmountRequest() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QString script = temporary.path() + "/mount-cryptex.py"; QFile file(script); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        MountCommand command; QString error;
        QVERIFY2(LauncherMount::makePrebootUnmountCommand("/usr/bin/true", script, &command, &error), qPrintable(error));
        QCOMPARE(command.program, QString("/usr/bin/true"));
        QCOMPARE(command.arguments, (QStringList{"-I", script, "--unmount"}));
        QVERIFY(!LauncherMount::makePrebootUnmountCommand("python3", script, &command, &error));
        QVERIFY(!LauncherMount::makePrebootUnmountCommand("/usr/bin/true", temporary.path() + "/missing.py", &command, &error));
        QVERIFY(!LauncherMount::makePrebootUnmountCommand("/usr/bin/true", "mount-cryptex.py", &command, &error));
    }
    void cryptexUnmountTargetsOnlyTheCryptexDirectory() {
        MountCommand command; QString error;
        QVERIFY2(LauncherMount::makeCryptexUnmountCommand("/usr/bin/true", "/run/user/1000/mount root", &command, &error), qPrintable(error));
        QCOMPARE(command.arguments, (QStringList{"-u", "--", "/run/user/1000/mount root/macos-cryptex-os"}));
        QVERIFY(!LauncherMount::makeCryptexUnmountCommand("/usr/bin/true", "/", &command, &error));
        QVERIFY(!LauncherMount::makeCryptexUnmountCommand("fusermount3", "/run/user/1000/mounts", &command, &error));
        QVERIFY(!LauncherMount::makeCryptexUnmountCommand("/usr/bin/true", "mounts", &command, &error));
    }
    void privilegeRunner_data() {
        QTest::addColumn<int>("exitCode"); QTest::addColumn<QString>("message");
        QTest::newRow("success") << 0 << "completed";
        QTest::newRow("cancelled") << 126 << "cancelled";
        QTest::newRow("denied") << 127 << "denied";
        QTest::newRow("mount-error") << 1 << "exit status 1";
    }
    void privilegeRunner() {
        QFETCH(int, exitCode); QFETCH(QString, message);
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QString script = temporary.path() + "/fake-pkexec";
        QFile file(script); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("#!/bin/sh\nprintf '%s\\n' \"$@\"\nexit " + QByteArray::number(exitCode) + "\n"); file.close();
        QVERIFY(QFile::setPermissions(script, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        MountRunner runner; QSignalSpy completed(&runner, &MountRunner::completed), output(&runner, &MountRunner::output);
        runner.start({script, {"/usr/bin/mount", "directory with spaces", "literal;value"}});
        QVERIFY(completed.wait(5000)); QCOMPARE(completed.size(), 1);
        QCOMPARE(completed.first()[0].toBool(), exitCode == 0);
        QVERIFY(completed.first()[1].toString().contains(message));
        QString text; for (const auto &event : output) text += event.first().toString();
        QVERIFY(text.contains("directory with spaces\nliteral;value"));
        QTest::qWait(30); QCOMPARE(completed.size(), 1);
    }
};
QTEST_GUILESS_MAIN(MountTest)
#include "mount_test.moc"
