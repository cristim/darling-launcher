#include "mount.h"
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class MountTest : public QObject {
    Q_OBJECT
private slots:
    void partitionDiscovery() {
        QString error;
        auto partitions = LauncherMount::parsePartitions(R"({"blockdevices":[{"path":"/dev/disk","fstype":null,"children":[{"path":"/dev/disk1","fstype":"apfs","mountpoints":[null]},{"path":"/dev/disk2","fstype":"hfsplus","label":"macOS","mountpoints":["/mnt/mac"]},{"path":"/dev/disk3","fstype":"ext4","mountpoints":[]}]}]})", &error);
        QVERIFY(error.isEmpty()); QCOMPARE(partitions.size(), 2);
        QCOMPARE(partitions[0].device, "/dev/disk1"); QVERIFY(!partitions[0].mounted);
        QCOMPARE(partitions[1].label, "macOS"); QVERIFY(partitions[1].mounted);
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
