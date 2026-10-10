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
