// SPDX-License-Identifier: GPL-3.0-or-later
#include "pipelineworkspace.h"
#include "discovery.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
namespace {
constexpr qint64 MaxTrace = 64 * 1024 * 1024, MaxScanned = 8 * 1024 * 1024;
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
// The compliance ruling (s2) allows names, library, arch, counts, call order and raw argument registers;
// anything else (caller address, PC, backtrace, stack, dereferenced memory) refuses the whole trace.
QString vetTrace(const QByteArray &data) {
    static const QRegularExpression allowed("^(?:symbol|sym|name|library|lib|image|framework|dylib|arch|count|calls|seq|sequence|order|first|position|kind|type|weak|mode"
        "|regs|args|arg_regs|fp_regs|words|x[0-8]|rdi|rsi|rdx|rcx|r8|r9|[qdv][0-7]|xmm[0-7]|time|ts|pid|version|app|schema)$");
    int n = 0;
    for (const QByteArray &line : data.split('\n')) {
        ++n;
        if (line.trimmed().isEmpty()) continue;
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (!doc.isObject()) return QString("Trace line %1 is not a JSON object.").arg(n);
        QList<QJsonValue> stack{doc.object()};
        while (!stack.isEmpty()) {
            const QJsonValue v = stack.takeLast();
            if (v.isArray()) for (const auto &x : v.toArray()) stack << x;
            if (!v.isObject()) continue;
            const QJsonObject o = v.toObject();
            for (auto it = o.begin(); it != o.end(); ++it) {
                const QString key = it.key().normalized(QString::NormalizationForm_KC).toLower();
                if (!allowed.match(key).hasMatch()) return QString("Trace line %1 records the field \"%2\", which the compliance ruling does not allow.").arg(n).arg(key.left(40));
                stack << it.value();
            }
        }
    }
    return {};
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
    static const QRegularExpression names("^(?:research|spec|impl|validate|recon|handoff|(?:research|spec|impl|validate|recon)\\.tainted)$");
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
// sha256 of a regular file of any size, streamed; empty if it cannot be read.
QString hashPath(const QString &path) {
    const int fd = ::open(local(path).constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return {};
    struct stat st {};
    QCryptographicHash hash(QCryptographicHash::Sha256); char buf[65536]; ssize_t n = 0; bool ok = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
    while (ok && (n = ::read(fd, buf, sizeof buf)) > 0) hash.addData(QByteArrayView(buf, n));
    ::close(fd);
    return ok && n == 0 ? QString::fromLatin1(hash.result().toHex()) : QString();
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
QString PipelineWorkspace::close() const {
    // A finished run is closed (mode 000) so later runs' agents cannot read it; chmod 700 it to inspect.
    return ::chmod(local(root).constData(), 0) == 0 ? QString() : "cannot close " + root + ": " + sysError();
}
QString PipelineWorkspace::defaultBase() { return LauncherDiscovery::dataRoot() + "/missing-symbol"; }
PipelineWorkspace PipelineWorkspace::create(const QString &base, QString *error) {
    PipelineWorkspace ws;
    // The base folder is never listable (0300), so a stage agent cannot find other runs' folders.
    if (!QDir().mkpath(base) || ::chmod(local(base).constData(), BaseMode) != 0) { *error = "cannot create " + base; return ws; }
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
    if (const QString problem = vetTrace(data); !problem.isEmpty())
        return problem + " The tracer may be non-compliant: the trace is not used; report it to compliance.";
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
    struct stat st {};
    if (::stat(local(QFileInfo(root).absolutePath()).constData(), &st) != 0) return "cannot check the base folder";
    baseInode = quint64(st.st_ino);
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
    locked.clear();
    ::close(rootFd);
    // The base folder must still be unlistable (other runs start concurrently, so its ctime is not compared).
    const QByteArray base = local(QFileInfo(root).absolutePath());
    struct stat st {};
    if (baseInode && (::lstat(base.constData(), &st) != 0 || quint64(st.st_ino) != baseInode || (st.st_mode & 07777) != BaseMode)) { *tampered = true; ::chmod(base.constData(), BaseMode); }
    baseInode = 0;
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
    for (const QString &dir : {stageDir("research"), root + "/research.tainted"})
        eachFile(dir, [&](const QString &path) {
            // Launcher-written and scratch files are not research material and would only cause false matches.
            static const QRegularExpression skip("(?:^|/)(?:\\.tmp/.*|BRIEF\\.md|REJECTED\\.txt)$");
            if (skip.match(path.mid(dir.size() + 1)).hasMatch() || QFileInfo(path).size() == 0) return;
            if (const QString h = hashPath(path); !h.isEmpty()) forbidden << h;
        });
    QString error;
    for (const auto &r : HandoffStore::releases(root, &error)) if (r.from == "research") forbidden << r.sha256;
    if (!error.isEmpty()) problems << error;
    const QString base = stageDir(stage);
    eachFile(base, [&](const QString &path) {
        const QString rel = path.mid(base.size() + 1);
        if (rel.startsWith("inputs/") && !delivered.contains(rel.mid(7))) problems << rel + " was not delivered by the launcher";
        if (QFileInfo(path).size() == 0) return; // every empty file has the same hash
        const QString h = hashPath(path);
        if (h.isEmpty()) { problems << rel + " cannot be read for checking"; return; }
        if (forbidden.contains(h)) { problems << rel + " is a copy of research material"; return; }
        if (QFileInfo(path).size() > MaxScanned) { problems << rel + " is too large to check for raw trace records"; return; }
        if (!HandoffCheck::rawTraceObjects(QString::fromUtf8(readPath(path, MaxScanned))).isEmpty()) problems << rel + " contains raw trace records";
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
int PipelineWorkspace::holdRunLock() const {
    const int fd = ::open(local(root + ".lock").constData(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0 && ::flock(fd, LOCK_EX | LOCK_NB) != 0) { ::close(fd); return -1; }
    return fd;
}
QStringList PipelineWorkspace::recover(const QString &base) {
    QStringList restored;
    if (::chmod(local(base).constData(), 0700) != 0) return restored; // listable only for this scan
    const QFileInfoList runs = QDir(base).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    ::chmod(local(base).constData(), BaseMode);
    for (const QFileInfo &run : runs) {
        struct stat st {};
        if (::lstat(local(run.filePath()).constData(), &st) != 0 || (st.st_mode & 07777) != 0100) continue;
        PipelineWorkspace probe; probe.root = run.filePath();
        const int lock = probe.holdRunLock(); // held by a live run, possibly in another launcher process
        if (lock < 0) continue;
        PipelineWorkspace ws; ws.root = run.filePath();
        const int rootFd = openPath(ws.root);
        if (rootFd < 0) continue;
        ::fchmodat(rootFd, ".", 0700, 0);
        for (const QString &name : lockable(rootFd)) ::fchmodat(rootFd, local(name).constData(), 0700, 0);
        ::close(rootFd);
        HandoffStore::appendEvent(ws.root, "INTERRUPTED", {{"reason", "the launcher stopped while a stage was running; modes restored"}});
        restored << ws.root;
        ::close(lock);
    }
    return restored;
}
