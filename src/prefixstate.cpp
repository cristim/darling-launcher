// SPDX-License-Identifier: GPL-3.0-or-later
#include "prefixstate.h"
#include <QFile>
#include <QFileInfo>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace PrefixState {
bool socketMayBeLive(const QString &socketPath) {
    const QByteArray path = QFile::encodeName(socketPath);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    // Darling cannot bind a socket path this long either, so a prefix like this is treated as running rather than probed.
    if (path.size() >= int(sizeof(addr.sun_path))) return true;
    memcpy(addr.sun_path, path.constData(), path.size());
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return true;
    const int rc = ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    const int err = errno;
    ::close(fd);
    return !(rc != 0 && (err == ENOENT || err == ECONNREFUSED));
}

bool prefixRunning(const QString &prefix, const ProcFs &procFs, QString *why) {
    auto running = [&](const QString &reason) { if (why) *why = reason; return true; };
    const int fd = ::open(QFile::encodeName(prefix + "/.init.pid").constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0 && errno != ENOENT) return running("cannot open .init.pid");
    if (fd >= 0) {
        char buf[34]{};
        const ssize_t n = ::read(fd, buf, 33);
        ::close(fd);
        if (n > 32) return running(".init.pid is too large to be a pid file");
        bool ok = false;
        const long pid = n > 0 ? QByteArray(buf, int(n)).trimmed().toLong(&ok) : 0;
        if (!ok || pid <= 0) return running(".init.pid does not hold a pid");
        const QByteArray proc = QFile::encodeName(procFs.procRoot + "/" + QString::number(pid));
        struct stat st;
        if (::lstat(proc.constData(), &st) != 0) {
            if (errno != ENOENT) return running("cannot inspect the /proc entry for the pid");
        } else {
            char target[4096];
            const ssize_t len = ::readlink((proc + "/exe").constData(), target, sizeof(target) - 1);
            if (len >= 0 && QFileInfo(QString::fromLocal8Bit(target, int(len))).fileName() == "darlingserver")
                return running("darlingserver is running for this prefix");
            return running("pid file names a process that is not a readable darlingserver (not treated as stale)");
        }
    }
    if (procFs.socketMayBeLive(prefix + "/.darlingserver.sock")) return running("darlingserver socket is live or unprobeable");
    return false;
}
}
