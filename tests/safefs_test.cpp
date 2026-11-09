// SPDX-License-Identifier: GPL-3.0-or-later
#include "safefs.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <sys/stat.h>
#include <unistd.h>

static QByteArray slurp(const QString &path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }

class SafeFsTest : public QObject {
    Q_OBJECT
private slots:
    void writesWithModesAndNoLeftovers() {
        QTemporaryDir d; QString err;
        QVERIFY2(SafeFs::writeFile(d.path(), "a/b/c.bin", "payload", 0600, 0700, false, &err), qPrintable(err));
        QCOMPARE(slurp(d.path() + "/a/b/c.bin"), QByteArray("payload"));
        struct stat st; QVERIFY(::stat((d.path() + "/a/b/c.bin").toUtf8().constData(), &st) == 0);
        QCOMPARE(int(st.st_mode & 0777), 0600);
        QVERIFY(::stat((d.path() + "/a/b").toUtf8().constData(), &st) == 0);
        QCOMPARE(int(st.st_mode & 0777), 0700);
        QCOMPARE(QDir(d.path() + "/a/b").entryList(QDir::Files | QDir::Hidden), QStringList({"c.bin"}));
    }
    void fileModeIsExactDespiteUmask() {
        QTemporaryDir d; QString err; const mode_t old = ::umask(022); struct stat st;
        QVERIFY(SafeFs::writeFile(d.path(), "f", "x", 0666, 0755, false, &err));
        ::umask(old);
        QVERIFY(::stat((d.path() + "/f").toUtf8().constData(), &st) == 0);
        QCOMPARE(int(st.st_mode & 0777), 0666);
    }
    void refusesExistingDestinationWithoutReplace() {
        QTemporaryDir d; QString err;
        QVERIFY(SafeFs::writeFile(d.path(), "f", "one", 0644, 0755, false, &err));
        QVERIFY(!SafeFs::writeFile(d.path(), "f", "two", 0644, 0755, false, &err));
        QCOMPARE(slurp(d.path() + "/f"), QByteArray("one"));
        QCOMPARE(QDir(d.path()).entryList(QDir::Files | QDir::Hidden).size(), 1); // temp file cleaned up
        QVERIFY(SafeFs::writeFile(d.path(), "f", "two", 0644, 0755, true, &err));
        QCOMPARE(slurp(d.path() + "/f"), QByteArray("two"));
    }
    void refusesSymlinkPlantedBeforeTheWalk() {
        QTemporaryDir d, evil; QString err;
        QVERIFY(QFile::link(evil.path(), d.path() + "/a"));
        QVERIFY(!SafeFs::writeFile(d.path(), "a/f", "x", 0644, 0755, false, &err));
        QVERIFY(QDir(evil.path()).isEmpty());
    }
    void symlinkSwappedAfterTheChainIsOpenedStillWritesToTheOriginalDirectory() {
        QTemporaryDir d, evil; QString err;
        QVERIFY(QDir().mkpath(d.path() + "/a/b"));
        SafeFs::Hooks hooks;
        hooks.afterDirsOpened = [&] {
            QVERIFY(QDir().rename(d.path() + "/a/b", d.path() + "/a/moved"));
            QVERIFY(QFile::link(evil.path(), d.path() + "/a/b"));
        };
        QVERIFY2(SafeFs::writeFile(d.path(), "a/b/f", "x", 0644, 0755, false, &err, hooks), qPrintable(err));
        QCOMPARE(slurp(d.path() + "/a/moved/f"), QByteArray("x"));
        QVERIFY(QDir(evil.path()).isEmpty());
    }
    void leafSymlinkIsNotFollowedOnReplace() {
        QTemporaryDir d, evil; QString err;
        QVERIFY(QFile::link(evil.path() + "/target", d.path() + "/f"));
        QVERIFY(SafeFs::writeFile(d.path(), "f", "x", 0644, 0755, true, &err));
        QVERIFY(!QFileInfo(d.path() + "/f").isSymLink());
        QVERIFY(QDir(evil.path()).isEmpty());
    }
    void rejectsUnconfinedPaths() {
        QTemporaryDir d; QString err;
        QVERIFY(!SafeFs::writeFile(d.path(), "../safefs-escape", "x", 0644, 0755, false, &err));
        QVERIFY(!SafeFs::writeFile(d.path(), "/abs", "x", 0644, 0755, false, &err));
    }
    void readMissingIsNotAnError() {
        QTemporaryDir d; QByteArray data; bool exists = true; QString err;
        QVERIFY(SafeFs::readFile(d.path(), "no/such/file", 100, &data, &exists, &err)); QVERIFY(!exists);
        QVERIFY(SafeFs::writeFile(d.path(), "x/f", "hello", 0644, 0755, false, &err));
        QVERIFY(SafeFs::readFile(d.path(), "x/f", 100, &data, &exists, &err)); QVERIFY(exists);
        QCOMPARE(data, QByteArray("hello"));
    }
    void readRefusesSymlinkFifoOversizedAndSymlinkedDir() {
        QTemporaryDir d, evil; QByteArray data; bool exists; QString err;
        QVERIFY(SafeFs::writeFile(evil.path(), "f", "secret", 0644, 0755, false, &err));
        QVERIFY(QFile::link(evil.path() + "/f", d.path() + "/link"));
        QVERIFY(!SafeFs::readFile(d.path(), "link", 100, &data, &exists, &err));
        QVERIFY(QFile::link(evil.path(), d.path() + "/dirlink"));
        QVERIFY(!SafeFs::readFile(d.path(), "dirlink/f", 100, &data, &exists, &err));
        QVERIFY(::mkfifo((d.path() + "/fifo").toUtf8().constData(), 0600) == 0);
        QVERIFY(!SafeFs::readFile(d.path(), "fifo", 100, &data, &exists, &err));
        QVERIFY(SafeFs::writeFile(d.path(), "big", QByteArray(200, 'x'), 0644, 0755, false, &err));
        QVERIFY(!SafeFs::readFile(d.path(), "big", 100, &data, &exists, &err));
    }
    void longestLeafAndNulAreHandled() {
        QTemporaryDir d; QString err; const QString leaf(255, 'n');
        QVERIFY2(SafeFs::writeFile(d.path(), "dir/" + leaf, "x", 0644, 0755, false, &err), qPrintable(err));
        QVERIFY(QFileInfo::exists(d.path() + "/dir/" + leaf));
        QVERIFY(!SafeFs::writeFile(d.path(), QString("a/b") + QChar(0) + "c", "x", 0644, 0755, false, &err));
        QByteArray data; bool exists;
        QVERIFY(!SafeFs::readFile(d.path(), QString("dir/f") + QChar(0), 10, &data, &exists, &err));
    }
    void renameInDirReplacesAndRefusesSymlinkedDirectory() {
        QTemporaryDir d, evil; QString err;
        QVERIFY(SafeFs::writeFile(d.path(), "s/a", "new", 0600, 0700, false, &err));
        QVERIFY(SafeFs::writeFile(d.path(), "s/b", "old", 0600, 0700, false, &err));
        QVERIFY2(SafeFs::renameInDir(d.path(), "s", "a", "b", &err), qPrintable(err));
        QCOMPARE(slurp(d.path() + "/s/b"), QByteArray("new")); QVERIFY(!QFileInfo::exists(d.path() + "/s/a"));
        QVERIFY(SafeFs::writeFile(evil.path(), "a", "x", 0600, 0700, false, &err));
        QVERIFY(QFile::link(evil.path(), d.path() + "/l"));
        QVERIFY(!SafeFs::renameInDir(d.path(), "l", "a", "b", &err));
        QVERIFY(QFileInfo::exists(evil.path() + "/a"));
    }
    void removeFileUnlinksOnlyThroughRealDirectories() {
        QTemporaryDir d, evil; QString err;
        QVERIFY(SafeFs::writeFile(d.path(), "s/a", "x", 0600, 0700, false, &err));
        QVERIFY2(SafeFs::removeFile(d.path(), "s/a", &err), qPrintable(err));
        QVERIFY(!QFileInfo::exists(d.path() + "/s/a"));
        QVERIFY(SafeFs::removeFile(d.path(), "s/a", &err)); // idempotent
        QVERIFY(SafeFs::removeFile(d.path(), "nodir/a", &err));
        QVERIFY(SafeFs::writeFile(evil.path(), "a", "x", 0600, 0700, false, &err));
        QVERIFY(QFile::link(evil.path(), d.path() + "/l"));
        QVERIFY(!SafeFs::removeFile(d.path(), "l/a", &err));
        QVERIFY(QFileInfo::exists(evil.path() + "/a"));
    }
    void removeDirectoryRemovesOnlyEmptyRealDirectories() {
        QTemporaryDir d, evil; QString err;
        QVERIFY(SafeFs::writeFile(d.path(), "s/a", "x", 0600, 0700, false, &err));
        QVERIFY(!SafeFs::removeDirectory(d.path(), "s", &err)); QVERIFY(QFileInfo::exists(d.path() + "/s/a"));
        QVERIFY(SafeFs::removeFile(d.path(), "s/a", &err));
        QVERIFY2(SafeFs::removeDirectory(d.path(), "s", &err), qPrintable(err));
        QVERIFY(!QFileInfo::exists(d.path() + "/s"));
        QVERIFY(SafeFs::removeDirectory(d.path(), "s", &err)); // idempotent
        QVERIFY(QDir().mkpath(evil.path() + "/sub")); QVERIFY(QFile::link(evil.path(), d.path() + "/l"));
        QVERIFY(!SafeFs::removeDirectory(d.path(), "l/sub", &err)); QVERIFY(QFileInfo::exists(evil.path() + "/sub"));
    }
    void lockExcludesAnotherHolder() {
        QTemporaryDir d; QString err;
        const int a = SafeFs::lockFile(d.path(), ".x/lock", 0700, &err); QVERIFY2(a >= 0, qPrintable(err));
        QVERIFY(SafeFs::lockFile(d.path(), ".x/lock", 0700, &err) < 0);
        ::close(a);
        const int b = SafeFs::lockFile(d.path(), ".x/lock", 0700, &err); QVERIFY(b >= 0); ::close(b);
    }
    void lockRefusesSymlinkedDirectory() {
        QTemporaryDir d, evil; QString err;
        QVERIFY(QFile::link(evil.path(), d.path() + "/.x"));
        QVERIFY(SafeFs::lockFile(d.path(), ".x/lock", 0700, &err) < 0);
        QVERIFY(QDir(evil.path()).isEmpty());
    }
    void openReadRefusesSymlinksAndNonRegularFiles() {
        QTemporaryDir d, evil; QString err; qint64 size = -1;
        QVERIFY(SafeFs::writeFile(d.path(), "a/f", "12345", 0600, 0700, false, &err));
        int fd = SafeFs::openRead(d.path(), "a/f", &size, &err); QVERIFY2(fd >= 0, qPrintable(err)); QCOMPARE(size, qint64(5)); ::close(fd);
        QVERIFY(SafeFs::writeFile(evil.path(), "t", "x", 0600, 0700, false, &err));
        QVERIFY(QFile::link(evil.path() + "/t", d.path() + "/leaf")); QVERIFY(QFile::link(evil.path(), d.path() + "/dir"));
        QVERIFY(SafeFs::openRead(d.path(), "leaf", &size, &err) < 0);
        QVERIFY(SafeFs::openRead(d.path(), "dir/t", &size, &err) < 0);
        QVERIFY(SafeFs::openRead(d.path(), "a", &size, &err) < 0);        // a directory
        QVERIFY(SafeFs::openRead(d.path(), "a/missing", &size, &err) < 0);
    }
    void openWriteCreatesExclusivelyOrResumes() {
        QTemporaryDir d, evil; QString err; qint64 existing = -1;
        int fd = SafeFs::openWrite(d.path(), "s/p.partial", 0600, 0700, false, &existing, &err); QVERIFY2(fd >= 0, qPrintable(err));
        QCOMPARE(existing, qint64(0)); QCOMPARE(::write(fd, "abc", 3), ssize_t(3));
        char back[3]; QCOMPARE(::pread(fd, back, 3, 0), ssize_t(3)); QCOMPARE(QByteArray(back, 3), QByteArray("abc")); ::close(fd);
        QVERIFY(SafeFs::openWrite(d.path(), "s/p.partial", 0600, 0700, false, &existing, &err) < 0); // exclusive
        fd = SafeFs::openWrite(d.path(), "s/p.partial", 0600, 0700, true, &existing, &err); QVERIFY(fd >= 0);
        QCOMPARE(existing, qint64(3)); ::close(fd);
        QVERIFY(QFile::link(evil.path(), d.path() + "/l"));
        QVERIFY(SafeFs::openWrite(d.path(), "l/p", 0600, 0700, true, &existing, &err) < 0);
        QVERIFY(QDir(evil.path()).isEmpty());
        QVERIFY(QFile::link(evil.path() + "/t", d.path() + "/s/link"));
        QVERIFY(SafeFs::openWrite(d.path(), "s/link", 0600, 0700, true, &existing, &err) < 0); // dangling symlink leaf
        QVERIFY(QDir(evil.path()).isEmpty());
    }
    void openWriteResumeRefusesHardLinkedAndFixesLooseMode() {
        QTemporaryDir d, other; QString err; qint64 existing = -1;
        QVERIFY(SafeFs::writeFile(other.path(), "victim", "secret", 0644, 0700, false, &err));
        QVERIFY(::link((other.path() + "/victim").toUtf8().constData(), (d.path() + "/p.partial").toUtf8().constData()) == 0);
        QVERIFY(SafeFs::openWrite(d.path(), "p.partial", 0600, 0700, true, &existing, &err) < 0);
        QVERIFY(err.contains("hard link"));
        QCOMPARE(slurp(other.path() + "/victim"), QByteArray("secret"));
        QVERIFY(SafeFs::writeFile(d.path(), "loose", "abc", 0666, 0700, false, &err));
        QVERIFY(::chmod((d.path() + "/loose").toUtf8().constData(), 0666) == 0);
        const int fd = SafeFs::openWrite(d.path(), "loose", 0600, 0700, true, &existing, &err); QVERIFY2(fd >= 0, qPrintable(err)); ::close(fd);
        QCOMPARE(existing, qint64(3));
        struct stat st; QVERIFY(::stat((d.path() + "/loose").toUtf8().constData(), &st) == 0); QCOMPARE(int(st.st_mode & 0777), 0600);
    }
    void renameNoReplaceKeepsTheTarget() {
        QTemporaryDir d; QString err;
        QVERIFY(SafeFs::writeFile(d.path(), "s/a", "new", 0600, 0700, false, &err));
        QVERIFY(SafeFs::writeFile(d.path(), "s/b", "old", 0600, 0700, false, &err));
        QVERIFY(!SafeFs::renameInDir(d.path(), "s", "a", "b", &err, true));
        QCOMPARE(slurp(d.path() + "/s/b"), QByteArray("old"));
        QVERIFY(SafeFs::renameInDir(d.path(), "s", "a", "b", &err));
        QCOMPARE(slurp(d.path() + "/s/b"), QByteArray("new"));
    }
    void checkPrivateDirRequiresOwnerOnlyAccess() {
        QTemporaryDir d, evil; QString err;
        QVERIFY(SafeFs::writeFile(d.path(), "p/f", "x", 0600, 0700, false, &err));
        QVERIFY2(SafeFs::checkPrivateDir(d.path(), "p", &err), qPrintable(err));
        QVERIFY(::chmod((d.path() + "/p").toUtf8().constData(), 0750) == 0);
        QVERIFY(!SafeFs::checkPrivateDir(d.path(), "p", &err)); QVERIFY(err.contains("0700"));
        QVERIFY(::chmod((d.path() + "/p").toUtf8().constData(), 0700) == 0);
        QVERIFY(QFile::link(evil.path(), d.path() + "/l"));
        QVERIFY(!SafeFs::checkPrivateDir(d.path(), "l", &err));
    }
};
QTEST_APPLESS_MAIN(SafeFsTest)
#include "safefs_test.moc"
