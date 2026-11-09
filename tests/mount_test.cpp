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
    void readOnlyCommandAndBoundaries() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        QString directory = temporary.path() + "/directory with spaces"; QVERIFY(QDir().mkpath(directory));
        MountCommand command; QString error; MacPartition partition{"/dev/nvme0n1p2", "apfs", {}, false};
        QVERIFY2(LauncherMount::makeCommand(partition, directory, MountBackend::Kernel, 2, 1000, 1000, "/usr/bin/true", "/usr/bin/true", &command, &error), qPrintable(error));
        QCOMPARE(command.arguments.last(), directory);
        QVERIFY(command.arguments.contains("ro,nodev,nosuid,noexec,uid=1000,gid=1000,vol=2"));
        QVERIFY(command.arguments.contains("-i")); QVERIFY(command.arguments.contains("--"));
        QVERIFY(!LauncherMount::makeCommand(partition, directory, MountBackend::Kernel, -1, 1000, 1000, "/usr/bin/true", "/usr/bin/true", &command, &error));
        QVERIFY(LauncherMount::makeCommand(partition, directory, MountBackend::ApfsFuse, 2, 1000, 1000, "/usr/bin/true", "/usr/bin/true", &command, &error));
        QVERIFY(command.arguments.contains("ro,nodev,nosuid,noexec,uid=1000,gid=1000,allow_other"));
        partition.device = "/dev/disk;touch /tmp/unwanted";
        QVERIFY(!LauncherMount::makeCommand(partition, directory, MountBackend::Kernel, 2, 1000, 1000, "/usr/bin/true", "/usr/bin/true", &command, &error));
        partition.device = "/dev/nvme0n1p2"; partition.mounted = true;
        QVERIFY(!LauncherMount::makeCommand(partition, directory, MountBackend::Kernel, 2, 1000, 1000, "/usr/bin/true", "/usr/bin/true", &command, &error));
        partition.mounted = false;
        QFile file(directory + "/existing"); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        QVERIFY(!LauncherMount::makeCommand(partition, directory, MountBackend::Kernel, 2, 1000, 1000, "/usr/bin/true", "/usr/bin/true", &command, &error));
        QVERIFY(QFile::remove(file.fileName()));
        QString link = temporary.path() + "/link"; QVERIFY(QFile::link(directory, link));
        QVERIFY(!LauncherMount::makeCommand(partition, link, MountBackend::Kernel, 2, 1000, 1000, "/usr/bin/true", "/usr/bin/true", &command, &error));
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
        QFile file(QStringLiteral(TEST_SOURCE_DIR) + "/tests/home_prefix_cases.json"); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto cases = QJsonDocument::fromJson(file.readAll()).array(); QVERIFY(!cases.isEmpty());
        auto fill = [&](QString text) { return text.replace("@ALIAS@", alias).replace("@REAL@", real); };
        for (const auto &value : cases) {
            const auto entry = value.toObject();
            QCOMPARE(LauncherMount::resolveHomePrefix(fill(entry["root"].toString()), fill(entry["home"].toString())), fill(entry["expect"].toString()));
        }
    }
    void passwdHomeIsCleanAndIgnoresTheEnvironment() {
        const QByteArray saved = qgetenv("HOME"); qputenv("HOME", "/somewhere/else/");
        const QString home = LauncherMount::passwdHome();
        if (saved.isNull()) qunsetenv("HOME"); else qputenv("HOME", saved);
        QVERIFY(!home.isEmpty()); QVERIFY(QDir::isAbsolutePath(home)); QCOMPARE(home, QDir::cleanPath(home)); QVERIFY(home != "/somewhere/else");
    }
    void rootRunFilesMustBeRootOwnedAndNotWritable() {
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
