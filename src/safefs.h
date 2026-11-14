// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QString>
#include <functional>
#include <sys/types.h>

// Writes below a prefix that the guest can modify. Every path component is opened with
// O_NOFOLLOW|O_DIRECTORY from a held descriptor, so a component swapped for a symlink after it was
// opened cannot redirect a write. `root` itself is opened following symlinks (it is the prefix the user chose).
namespace SafeFs {
// Owns a descriptor and closes it.
struct Fd {
    int fd;
    explicit Fd(int f = -1) : fd(f) {}
    ~Fd();
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    int release() { const int f = fd; fd = -1; return f; }
};
// Reads exactly len bytes at off; false on error or end of file.
bool preadFull(int fd, char *buf, qint64 len, qint64 off);
struct Hooks {
    std::function<void()> afterDirsOpened; // test seam: runs after the directory chain is held, before the file is created
};
// Atomic write via O_EXCL temp file + renameat. Without `replace` an existing destination is refused.
bool writeFile(const QString &root, const QString &relPath, const QByteArray &data, mode_t fileMode, mode_t dirMode,
               bool replace, QString *error, const Hooks &hooks = {});
// A missing file or directory chain is not an error: *exists is false. A non-regular or oversized file is.
bool readFile(const QString &root, const QString &relPath, qint64 maxBytes, QByteArray *data, bool *exists, QString *error);
// Renames a file within one directory (replacing the target unless noReplace); the directory is reached without following symlinks.
bool renameInDir(const QString &root, const QString &dirRelPath, const QString &from, const QString &to, QString *error,
                 bool noReplace = false);
// Opens a regular file for reading (no symlink in any component, no FIFO/device); returns the descriptor or -1. *size is its length.
int openRead(const QString &root, const QString &relPath, qint64 *size, QString *error);
// Opens a regular file for reading and writing (a resumed copy re-reads what it wrote) without following symlinks, creating missing directories with dirMode.
// Without `resume` the file must not exist (O_EXCL). With it an existing regular file is kept and *existingSize reports
// its length, so a copy can continue; a new file has size 0. Returns the descriptor or -1.
int openWrite(const QString &root, const QString &relPath, mode_t fileMode, mode_t dirMode, bool resume,
              qint64 *existingSize, QString *error);
// Creates relPath and any missing parents (dirMode) without following a symlink; an existing real directory is success.
bool ensureDirectory(const QString &root, const QString &relPath, mode_t dirMode, QString *error);
// Creates the symlink relPath -> target (stored verbatim) after creating missing parents. An existing symlink with the same
// target is success; anything else at relPath is refused.
bool symlinkAt(const QString &root, const QString &relPath, const QString &target, mode_t dirMode, QString *error);
// Succeeds only for a directory owned by this user with no group or other permission bits.
bool checkPrivateDir(const QString &root, const QString &relPath, QString *error);
// Unlinks a file (never a directory, never through a symlinked parent). Already absent is success.
bool removeFile(const QString &root, const QString &relPath, QString *error);
// Removes an empty directory (never through a symlinked parent). Already absent is success.
bool removeDirectory(const QString &root, const QString &relPath, QString *error);
// flock(LOCK_EX|LOCK_NB) on a regular file; returns the descriptor (close it to unlock) or -1.
int lockFile(const QString &root, const QString &relPath, mode_t dirMode, QString *error);
}
