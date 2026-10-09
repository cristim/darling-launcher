// SPDX-License-Identifier: GPL-3.0-or-later
#include "pipelineworkspace.h"
#include "discovery.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
namespace {
constexpr qint64 MaxTrace = 64 * 1024 * 1024, MaxScanned = 1000000;
QByteArray local(const QString &s) { return s.toLocal8Bit(); }
QString sysError() { return QString::fromLocal8Bit(strerror(errno)); }
QString sha256(const QByteArray &b) { return QString::fromLatin1(QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex()); }
// Reads a regular, non-symlink file; empty optional-like result via ok flag.
QByteArray readRegular(int dir, const QString &name, qint64 limit, QString *error) {
    const int fd = ::openat(dir, local(name).constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { *error = "cannot open " + name + ": " + sysError(); return {}; }
    struct stat st {};
    QByteArray data;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) *error = name + " is not a regular file";
    else if (st.st_size > limit) *error = name + " is larger than " + QString::number(limit) + " bytes";
    else {
        data.resize(st.st_size);
        qint64 got = 0;
        while (got < data.size()) { const ssize_t n = ::read(fd, data.data() + got, size_t(data.size() - got)); if (n <= 0) break; got += n; }
        if (got != data.size()) *error = "short read of " + name;
    }
    ::close(fd);
    return error->isEmpty() ? data : QByteArray();
}
QString writeNew(int dir, const QString &name, const QByteArray &data) {
    const int fd = ::openat(dir, local(name).constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return "cannot create " + name + ": " + sysError();
    const bool ok = ::write(fd, data.constData(), size_t(data.size())) == data.size() && ::fchmod(fd, 0600) == 0 && ::fsync(fd) == 0;
    ::close(fd);
    return ok ? QString() : "cannot write " + name;
}
int openDir(const QString &path) { return ::open(local(path).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC); }
// For *at() calls on a run root that may be locked to 0100: O_PATH needs no read permission.
int openPath(const QString &path) { return ::open(local(path).constData(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC); }
QString makePrivateDir(int at, const QString &name) {
    if (::mkdirat(at, local(name).constData(), 0700) != 0 && errno != EEXIST) return "cannot create " + name + ": " + sysError();
    struct stat st {};
    if (::fstatat(at, local(name).constData(), &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(st.st_mode)) return name + " is not a directory";
    return {};
}
// Directories locked while another stage runs: run-root children other than the active stage.
QStringList lockable(int rootFd) {
    static const QRegularExpression names("^(?:research|spec|impl|validate|handoff|(?:research|spec|impl|validate)\\.tainted)$");
    QStringList out;
    for (const QString &n : QDir(QString("/proc/self/fd/%1").arg(rootFd)).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks))
        if (names.match(n).hasMatch()) out << n;
    return out;
}
// Visits regular files under dir without following symlinks.
template <typename F> void eachFile(const QString &dir, F visit) {
    QDirIterator it(dir, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext()) visit(it.next());
}
QByteArray readPath(const QString &path, qint64 limit) {
    QString error; const QFileInfo info(path);
    const int dir = openDir(info.absolutePath());
    if (dir < 0) return {};
    const QByteArray data = readRegular(dir, info.fileName(), limit, &error);
    ::close(dir);
    return data;
}
}
QString PipelineWorkspace::defaultBase() { return LauncherDiscovery::dataRoot() + "/missing-symbol"; }
PipelineWorkspace PipelineWorkspace::create(const QString &base, QString *error) {
    PipelineWorkspace ws;
    if (!QDir().mkpath(base) || !QFile::setPermissions(base, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) { *error = "cannot create " + base; return ws; }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces), root = base + '/' + id;
    if (::mkdir(local(root).constData(), 0700) != 0) { *error = "cannot create " + root + ": " + sysError(); return ws; }
    const int fd = openDir(root);
    if (fd < 0) { *error = "cannot open " + root; return ws; }
    for (const QString &d : HandoffStore::Stages + QStringList{"handoff"}) if (const QString e = makePrivateDir(fd, d); !e.isEmpty() && error->isEmpty()) *error = e;
    ::close(fd);
    if (error->isEmpty()) { ws.root = root; ws.runId = id; }
    return ws;
}
QString PipelineWorkspace::importTrace(const QString &path) const {
    QString error; const QFileInfo info(path);
    const int dir = openDir(info.absolutePath());
    if (dir < 0) return "cannot open the trace folder " + info.absolutePath();
    const QByteArray data = readRegular(dir, info.fileName(), MaxTrace, &error);
    ::close(dir);
    if (!error.isEmpty()) return error;
    if (!HandoffCheck::forbiddenTraceFields(QString::fromUtf8(data)).isEmpty())
        return "The trace records caller addresses, PC or backtraces. The tracer is non-compliant: the trace is not used; report it to compliance.";
    const int research = openDir(stageDir("research"));
    if (research < 0) return "cannot open the research workspace";
    QString result = makePrivateDir(research, "trace");
    const int trace = result.isEmpty() ? ::openat(research, "trace", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
    if (result.isEmpty()) result = trace < 0 ? "cannot open research/trace" : writeNew(trace, "T0.jsonl", data);
    if (trace >= 0) ::close(trace);
    ::close(research);
    return result;
}
QString PipelineWorkspace::deliver(const HandoffStore::Release &release, const QString &stage) const {
    if (release.name.contains('/')) return "invalid release name";
    QString error;
    const int hand = openDir(root + "/handoff");
    if (hand < 0) return "cannot open the handoff folder";
    const QByteArray data = readRegular(hand, release.name, HandoffStore::MaxInput, &error);
    ::close(hand);
    if (!error.isEmpty()) return error;
    if (sha256(data) != release.sha256) return release.name + " no longer matches its audit record";
    const int dir = openDir(stageDir(stage));
    if (dir < 0) return "cannot open the " + stage + " workspace";
    QString result = makePrivateDir(dir, "inputs");
    const int inputs = result.isEmpty() ? ::openat(dir, "inputs", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
    if (result.isEmpty()) result = inputs < 0 ? "cannot open " + stage + "/inputs" : writeNew(inputs, release.name, data);
    if (inputs >= 0) ::close(inputs);
    ::close(dir);
    return result;
}
QString PipelineWorkspace::lockFor(const QString &stage) {
    const int rootFd = openPath(root);
    if (rootFd < 0) return "cannot open " + root;
    QString error;
    auto lock = [&](const QString &name, unsigned mode) {
        struct stat st {};
        const QByteArray n = local(name);
        if (::fchmodat(rootFd, n.constData(), mode, 0) != 0 || ::fstatat(rootFd, n.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0) { error = "cannot lock " + name + ": " + sysError(); return; }
        locked.insert(name, {quint64(st.st_ino), qint64(st.st_ctim.tv_sec), qint64(st.st_ctim.tv_nsec), unsigned(st.st_mode & 07777)});
    };
    for (const QString &name : lockable(rootFd)) if (name != stage) lock(name, 0);
    lock(".", 0100);
    ::close(rootFd);
    return error;
}
QString PipelineWorkspace::unlock(bool *tampered) {
    *tampered = false;
    const int rootFd = openPath(root);
    if (rootFd < 0) return "cannot open " + root;
    QString error;
    for (auto it = locked.begin(); it != locked.end(); ++it) {
        struct stat st {};
        const QByteArray n = local(it.key());
        if (::fstatat(rootFd, n.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(st.st_mode) || quint64(st.st_ino) != it->inode
            || st.st_ctim.tv_sec != it->ctimeSec || st.st_ctim.tv_nsec != it->ctimeNsec || unsigned(st.st_mode & 07777) != it->mode) { *tampered = true; if (!S_ISDIR(st.st_mode)) continue; }
        if (::fchmodat(rootFd, n.constData(), 0700, 0) != 0) error = "cannot unlock " + it.key() + ": " + sysError();
    }
    // A directory created in the run root during the stage is tampering too (the root was not writable).
    for (const QString &name : QDir(root).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
        if (!locked.contains(name) && !HandoffStore::Stages.contains(name)) *tampered = true;
    locked.clear();
    ::close(rootFd);
    return error;
}
QString PipelineWorkspace::taint(const QString &stage, const QString &agentId) const {
    static const QRegularExpression safe("^[A-Za-z0-9-]{1,80}$");
    if (!HandoffStore::Stages.contains(stage) || !safe.match(agentId).hasMatch()) return "invalid stage or agent id";
    const int rootFd = openDir(root);
    if (rootFd < 0) return "cannot open " + root;
    const QString dir = stage + ".tainted";
    QString error = makePrivateDir(rootFd, dir);
    const int taintFd = error.isEmpty() ? ::openat(rootFd, local(dir).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
    if (error.isEmpty() && (taintFd < 0 || ::renameat(rootFd, local(stage).constData(), taintFd, local(agentId).constData()) != 0)) error = "cannot move the tainted " + stage + " workspace: " + sysError();
    if (error.isEmpty()) error = makePrivateDir(rootFd, stage);
    if (taintFd >= 0) ::close(taintFd);
    ::close(rootFd);
    return error;
}
QStringList PipelineWorkspace::firewall(const QString &stage, const QStringList &delivered) const {
    QSet<QString> forbidden; QStringList problems;
    eachFile(stageDir("research"), [&](const QString &path) { const QByteArray d = readPath(path, MaxTrace); if (!d.isEmpty()) forbidden << sha256(d); });
    QString error;
    for (const auto &r : HandoffStore::releases(root, &error)) if (r.from == "research") forbidden << r.sha256;
    if (!error.isEmpty()) problems << error;
    const QString base = stageDir(stage);
    eachFile(base, [&](const QString &path) {
        const QString rel = path.mid(base.size() + 1);
        if (rel.startsWith("inputs/") && !delivered.contains(rel.mid(7))) problems << rel + " was not delivered by the launcher";
        const QByteArray d = readPath(path, MaxScanned);
        if (d.isEmpty()) return;
        if (forbidden.contains(sha256(d))) problems << rel + " is a copy of research material";
        else if (!HandoffCheck::rawTraceObjects(QString::fromUtf8(d)).isEmpty()) problems << rel + " contains raw trace records";
    });
    return problems;
}
QList<qint64> PipelineWorkspace::processesUsing(const QString &dir) {
    QList<qint64> pids;
    const QString canonical = QFileInfo(dir).canonicalFilePath();
    if (canonical.isEmpty()) return pids;
    auto inside = [&](const QString &p) { return p == canonical || p.startsWith(canonical + '/'); };
    for (const QString &pid : QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool numeric = false; const qint64 n = pid.toLongLong(&numeric);
        if (!numeric || n == ::getpid()) continue;
        bool hit = inside(QFileInfo("/proc/" + pid + "/cwd").symLinkTarget());
        for (const QString &fd : hit ? QStringList() : QDir("/proc/" + pid + "/fd").entryList(QDir::System | QDir::NoDotAndDotDot))
            if (inside(QFileInfo("/proc/" + pid + "/fd/" + fd).symLinkTarget())) { hit = true; break; }
        if (hit) pids << n;
    }
    return pids;
}
QStringList PipelineWorkspace::recover(const QString &base) {
    QStringList restored;
    for (const QFileInfo &run : QDir(base).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
        struct stat st {};
        if (::lstat(local(run.filePath()).constData(), &st) != 0 || (st.st_mode & 07777) != 0100) continue;
        PipelineWorkspace ws; ws.root = run.filePath();
        const int rootFd = openPath(ws.root);
        if (rootFd < 0) continue;
        ::fchmodat(rootFd, ".", 0700, 0);
        for (const QString &name : lockable(rootFd)) ::fchmodat(rootFd, local(name).constData(), 0700, 0);
        ::close(rootFd);
        HandoffStore::appendEvent(ws.root, "INTERRUPTED", {{"reason", "the launcher stopped while a stage was running; modes restored"}});
        restored << ws.root;
    }
    return restored;
}
