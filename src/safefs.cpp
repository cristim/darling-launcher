// SPDX-License-Identifier: GPL-3.0-or-later
#include "safefs.h"
#include "core.h"
#include <QFile>
#include <QRandomGenerator>
#include <QStringList>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using LauncherCore::fail;
struct Fd {
    int fd;
    explicit Fd(int f = -1) : fd(f) {}
    ~Fd() { if (fd >= 0) ::close(fd); }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    int release() { const int f = fd; fd = -1; return f; }
};
QString sysError(const QString &what) { return what + ": " + QString::fromLocal8Bit(strerror(errno)); }

// Opens root then each component of `dirs` without following symlinks. *missing is set (and -1 returned
// without an error) when a component does not exist and create is off.
int openDirs(const QString &root, const QStringList &dirs, bool create, mode_t dirMode, bool *missing, QString *error) {
    if (missing) *missing = false;
    int cur = ::open(QFile::encodeName(root).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    // never the absolute path: errors reach logs and the MCP server
    if (cur < 0) { fail(error, sysError("Cannot open the base directory")); return -1; }
    for (const QString &name : dirs) {
        const QByteArray n = name.toUtf8();
        const auto openNext = [&] { return ::openat(cur, n.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC); };
        int next = openNext();
        if (next < 0 && errno == ENOENT && create) {
            if (::mkdirat(cur, n.constData(), dirMode) != 0 && errno != EEXIST) { fail(error, sysError("Cannot create " + name)); ::close(cur); return -1; }
            next = openNext();
        }
        const int err = errno;
        ::close(cur);
        if (next < 0) {
            errno = err;
            if (err == ENOENT && missing) *missing = true; else fail(error, sysError("Refusing path component " + name));
            return -1;
        }
        cur = next;
    }
    return cur;
}
bool split(const QString &relPath, QStringList *dirs, QString *leaf, QString *error) {
    if (relPath.contains(QChar(0)) || !LauncherCore::confinedRelative(relPath)) return fail(error, "Path is not confined: " + relPath);
    *dirs = relPath.split('/'); *leaf = dirs->takeLast();
    return true;
}
}

namespace SafeFs {
bool writeFile(const QString &root, const QString &relPath, const QByteArray &data, mode_t fileMode, mode_t dirMode,
               bool replace, QString *error, const Hooks &hooks) {
    QStringList dirs; QString leaf;
    if (!split(relPath, &dirs, &leaf, error)) return false;
    Fd dir(openDirs(root, dirs, true, dirMode, nullptr, error));
    if (dir.fd < 0) return false;
    if (hooks.afterDirsOpened) hooks.afterDirsOpened();
    const QByteArray name = leaf.toUtf8();
    // fixed length: a temp name derived from a 255-byte leaf would exceed NAME_MAX
    const QByteArray tmp = ".tmp." + QByteArray::number(QRandomGenerator::system()->generate64(), 16).rightJustified(16, '0');
    Fd file(::openat(dir.fd, tmp.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, fileMode));
    if (file.fd < 0) return fail(error, sysError("Cannot create temporary file for " + relPath));
    bool ok = ::fchmod(file.fd, fileMode) == 0;
    for (qint64 off = 0; ok && off < data.size();) {
        const ssize_t n = ::write(file.fd, data.constData() + off, size_t(data.size() - off));
        if (n < 0 && errno == EINTR) continue;
        ok = n > 0; off += n;
    }
    ok = ok && ::fsync(file.fd) == 0;
    if (ok) ok = replace ? ::renameat(dir.fd, tmp.constData(), dir.fd, name.constData()) == 0
                         : ::renameat2(dir.fd, tmp.constData(), dir.fd, name.constData(), RENAME_NOREPLACE) == 0;
    if (!ok) { const QString msg = sysError("Cannot write " + relPath); ::unlinkat(dir.fd, tmp.constData(), 0); return fail(error, msg); }
    ::fsync(dir.fd);
    return true;
}

bool readFile(const QString &root, const QString &relPath, qint64 maxBytes, QByteArray *data, bool *exists, QString *error) {
    QStringList dirs; QString leaf; bool missing = false;
    *exists = false; data->clear();
    if (!split(relPath, &dirs, &leaf, error)) return false;
    Fd dir(openDirs(root, dirs, false, 0, &missing, error));
    if (dir.fd < 0) return missing;
    Fd file(::openat(dir.fd, leaf.toUtf8().constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (file.fd < 0) return errno == ENOENT ? true : fail(error, sysError("Cannot open " + relPath));
    struct stat st;
    if (::fstat(file.fd, &st) != 0 || !S_ISREG(st.st_mode)) return fail(error, relPath + " is not a regular file");
    if (st.st_size > maxBytes) return fail(error, relPath + " is too large");
    QByteArray buf(int(st.st_size), Qt::Uninitialized);
    for (qint64 off = 0; off < buf.size();) {
        const ssize_t n = ::read(file.fd, buf.data() + off, size_t(buf.size() - off));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return fail(error, sysError("Cannot read " + relPath));
        off += n;
    }
    *data = buf; *exists = true;
    return true;
}

bool renameInDir(const QString &root, const QString &dirRelPath, const QString &from, const QString &to, QString *error,
                 bool noReplace) {
    QStringList dirs; QString leaf;
    if (!split(dirRelPath + "/x", &dirs, &leaf, error) || from.contains('/') || to.contains('/')) return fail(error, "Bad rename request");
    Fd dir(openDirs(root, dirs, false, 0, nullptr, error));
    if (dir.fd < 0) return false;
    const int rc = noReplace ? ::renameat2(dir.fd, from.toUtf8().constData(), dir.fd, to.toUtf8().constData(), RENAME_NOREPLACE)
                             : ::renameat(dir.fd, from.toUtf8().constData(), dir.fd, to.toUtf8().constData());
    if (rc != 0) return fail(error, sysError("Cannot rename " + from));
    ::fsync(dir.fd);
    return true;
}

bool removeFile(const QString &root, const QString &relPath, QString *error) {
    QStringList dirs; QString leaf; bool missing = false;
    if (!split(relPath, &dirs, &leaf, error)) return false;
    Fd dir(openDirs(root, dirs, false, 0, &missing, error));
    if (dir.fd < 0) return missing;
    if (::unlinkat(dir.fd, leaf.toUtf8().constData(), 0) != 0 && errno != ENOENT) return fail(error, sysError("Cannot remove " + relPath));
    ::fsync(dir.fd);
    return true;
}

bool removeDirectory(const QString &root, const QString &relPath, QString *error) {
    QStringList dirs; QString leaf; bool missing = false;
    if (!split(relPath, &dirs, &leaf, error)) return false;
    Fd dir(openDirs(root, dirs, false, 0, &missing, error));
    if (dir.fd < 0) return missing;
    if (::unlinkat(dir.fd, leaf.toUtf8().constData(), AT_REMOVEDIR) != 0 && errno != ENOENT) return fail(error, sysError("Cannot remove directory " + relPath));
    return true;
}

int lockFile(const QString &root, const QString &relPath, mode_t dirMode, QString *error) {
    QStringList dirs; QString leaf;
    if (!split(relPath, &dirs, &leaf, error)) return -1;
    Fd dir(openDirs(root, dirs, true, dirMode, nullptr, error));
    if (dir.fd < 0) return -1;
    Fd file(::openat(dir.fd, leaf.toUtf8().constData(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600));
    struct stat st;
    if (file.fd < 0 || ::fstat(file.fd, &st) != 0 || !S_ISREG(st.st_mode)) { fail(error, "Cannot open lock file " + relPath); return -1; }
    if (::flock(file.fd, LOCK_EX | LOCK_NB) != 0) { fail(error, "Another launcher process holds " + relPath); return -1; }
    return file.release();
}

int openRead(const QString &root, const QString &relPath, qint64 *size, QString *error) {
    QStringList dirs; QString leaf; bool missing = false;
    if (!split(relPath, &dirs, &leaf, error)) return -1;
    Fd dir(openDirs(root, dirs, false, 0, &missing, error));
    if (dir.fd < 0) { if (missing) fail(error, "No such file: " + relPath); return -1; }
    Fd file(::openat(dir.fd, leaf.toUtf8().constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (file.fd < 0) { fail(error, sysError("Cannot open " + relPath)); return -1; }
    struct stat st;
    if (::fstat(file.fd, &st) != 0 || !S_ISREG(st.st_mode)) { fail(error, relPath + " is not a regular file"); return -1; }
    if (size) *size = st.st_size;
    return file.release();
}

int openWrite(const QString &root, const QString &relPath, mode_t fileMode, mode_t dirMode, bool resume,
              qint64 *existingSize, QString *error) {
    QStringList dirs; QString leaf;
    if (!split(relPath, &dirs, &leaf, error)) return -1;
    Fd dir(openDirs(root, dirs, true, dirMode, nullptr, error));
    if (dir.fd < 0) return -1;
    const QByteArray name = leaf.toUtf8();
    Fd file(::openat(dir.fd, name.constData(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | (resume ? 0 : O_EXCL), fileMode));
    if (file.fd < 0) { fail(error, sysError("Cannot open " + relPath + " for writing")); return -1; }
    struct stat st;
    if (::fstat(file.fd, &st) != 0 || !S_ISREG(st.st_mode)) { fail(error, relPath + " is not a regular file"); return -1; }
    // A file planted by the guest (same uid, so uid alone proves little) can be a hard link to something else or carry loose modes.
    if (st.st_uid != ::geteuid()) { fail(error, relPath + " is not owned by this user"); return -1; }
    if (st.st_nlink != 1) { fail(error, relPath + " has more than one hard link"); return -1; }
    if (::fchmod(file.fd, fileMode) != 0) { fail(error, sysError("Cannot set the mode of " + relPath)); return -1; }
    if (existingSize) *existingSize = st.st_size;
    return file.release();
}

bool checkPrivateDir(const QString &root, const QString &relPath, QString *error) {
    QStringList dirs; QString leaf;
    if (!split(relPath + "/x", &dirs, &leaf, error)) return false;
    Fd dir(openDirs(root, dirs, false, 0, nullptr, error));
    if (dir.fd < 0) return false;
    struct stat st;
    if (::fstat(dir.fd, &st) != 0) return fail(error, sysError("Cannot stat " + relPath));
    if (st.st_uid != ::geteuid()) return fail(error, relPath + " is not owned by this user");
    if (st.st_mode & 077) return fail(error, relPath + " must not be accessible to group or others (mode 0700)");
    return true;
}
}
