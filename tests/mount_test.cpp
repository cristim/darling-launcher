// SPDX-License-Identifier: GPL-3.0-or-later
#include "mount.h"
#include <QDir>
#include <QFile>
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
