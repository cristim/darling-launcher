// SPDX-License-Identifier: GPL-3.0-or-later
#include "handoffstore.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <map>
namespace {
const char *Audit = "AUDIT.jsonl", *Manifest = "MANIFEST.txt";
struct Unsafe { QString message; };
struct Fd {
    int fd = -1;
    explicit Fd(int f = -1) : fd(f) {}
    Fd(Fd &&o) noexcept : fd(o.fd) { o.fd = -1; }
    Fd &operator=(Fd &&o) noexcept { std::swap(fd, o.fd); return *this; }
    ~Fd() { if (fd >= 0) ::close(fd); }
};
QByteArray local(const QString &s) { return s.toLocal8Bit(); }
QString sysError() { return QString::fromLocal8Bit(strerror(errno)); }
QString sha256(const QByteArray &b) { return QString::fromLatin1(QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex()); }
QString cleanRoot(const QString &runRoot) {
    QString root = runRoot;
    while (root.size() > 1 && root.endsWith('/')) root.chop(1); // a trailing slash would make O_NOFOLLOW follow
    return root;
}
Fd openDir(int at, const QString &name, const QString &shown) {
    Fd d(::openat(at, local(name).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (d.fd < 0) throw Unsafe{errno == ELOOP || errno == ENOTDIR ? "refusing symlink or non-directory: " + shown : (errno == ENOENT ? "missing directory: " : "cannot open " ) + shown + (errno == ENOENT ? QString() : ": " + sysError())};
    return d;
}
void requirePrivate(int fd, const QString &shown) {
    struct stat st {};
    if (::fstat(fd, &st) != 0) throw Unsafe{"cannot stat " + shown};
    if (st.st_mode & 077) throw Unsafe{"directory is group/world accessible (chmod 700 it): " + shown};
}
QStringList listDir(int fd) {
    Fd copy(::dup(fd)); QStringList names;
    DIR *dir = copy.fd >= 0 ? ::fdopendir(copy.fd) : nullptr;
    if (!dir) throw Unsafe{"cannot list directory: " + sysError()};
    copy.fd = -1; ::rewinddir(dir);
    while (dirent *e = ::readdir(dir)) { const QString n = QString::fromLocal8Bit(e->d_name); if (n != "." && n != "..") names << n; }
    ::closedir(dir); names.sort();
    return names;
}
QByteArray readAll(int fd, qint64 limit) {
    QByteArray data; char buf[65536];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0) { if (errno == EINTR) continue; throw Unsafe{"read failed: " + sysError()}; }
        if (n == 0 || (data += QByteArray(buf, n)).size() > limit) return data;
    }
}
Fd openFile(int dir, const char *name, int flags) {
    Fd f(::openat(dir, name, flags | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (f.fd < 0) throw Unsafe{(errno == ELOOP ? "refusing symlink: " : "cannot open ") + QString::fromLatin1(name) + (errno == ELOOP ? QString() : ": " + sysError())};
    return f;
}
void writeAll(int fd, const QByteArray &b) {
    if (::write(fd, b.constData(), size_t(b.size())) != b.size()) throw Unsafe{"write failed: " + sysError()};
}
// Run root: exists, not a symlink, private, with at least one private stage workspace (prepare_run_root).
Fd openRunRoot(const QString &runRoot) {
    const QString root = cleanRoot(runRoot);
    struct stat st {};
    if (::lstat(local(root).constData(), &st) != 0) throw Unsafe{"run root does not exist (typo?): " + root};
    if (S_ISLNK(st.st_mode)) throw Unsafe{"run root is a symlink: " + runRoot};
    Fd fd = openDir(AT_FDCWD, root, root);
    requirePrivate(fd.fd, root);
    bool any = false;
    for (const QString &stage : HandoffStore::Stages) {
        if (::fstatat(fd.fd, local(stage).constData(), &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
        if (S_ISLNK(st.st_mode)) throw Unsafe{"refusing symlink: " + root + '/' + stage};
        if (!S_ISDIR(st.st_mode)) continue;
        any = true;
        if (st.st_mode & 077) throw Unsafe{"directory is group/world accessible (chmod 700 it): " + root + '/' + stage};
    }
    if (!any) throw Unsafe{"run root has no stage workspace dir (research/ spec/ impl/ validate/): " + root};
    return fd;
}
Fd openHandoff(int rootFd, const QString &root, bool create) {
    if (create && ::mkdirat(rootFd, "handoff", 0700) != 0 && errno != EEXIST) throw Unsafe{"cannot create handoff dir: " + sysError()};
    Fd fd = openDir(rootFd, "handoff", root + "/handoff");
    requirePrivate(fd.fd, root + "/handoff");
    return fd;
}
QByteArray readInput(const QString &root, const QString &from, const QString &file) {
    struct stat st {};
    if (::lstat(local(file).constData(), &st) == 0 && S_ISLNK(st.st_mode)) throw Unsafe{"input file is a symlink: " + file};
    char ws[PATH_MAX], real[PATH_MAX];
    if (!::realpath(local(root + '/' + from).constData(), ws)) throw Unsafe{"stage workspace missing: " + root + '/' + from};
    if (!::realpath(local(file).constData(), real)) throw Unsafe{"cannot open input " + file + ": " + sysError()};
    const QString w = QString::fromLocal8Bit(ws), r = QString::fromLocal8Bit(real);
    if (!r.startsWith(w + '/')) throw Unsafe{"input file is outside the " + from + " workspace (" + w + "): " + file};
    Fd fd(::open(real, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (fd.fd < 0) throw Unsafe{"cannot open input " + file + ": " + sysError()};
    if (::fstat(fd.fd, &st) != 0 || !S_ISREG(st.st_mode)) throw Unsafe{"input is not a regular file: " + file};
    if (st.st_nlink != 1) throw Unsafe{"input has other hard links: " + file}; // stricter than the oracle
    const QByteArray data = readAll(fd.fd, HandoffStore::MaxInput);
    if (data.size() > HandoffStore::MaxInput) throw Unsafe{"input larger than " + QString::number(HandoffStore::MaxInput) + " bytes: " + file};
    return data;
}
QList<QByteArray> readRecords(int auditFd) {
    struct stat st {};
    if (::fstat(auditFd, &st) != 0) throw Unsafe{"cannot stat AUDIT.jsonl"};
    QByteArray raw(st.st_size, '\0');
    if (st.st_size && ::pread(auditFd, raw.data(), size_t(st.st_size), 0) != st.st_size) throw Unsafe{"cannot read AUDIT.jsonl"};
    QList<QByteArray> lines; int i = 0;
    for (const QByteArray &l : raw.split('\n')) {
        if (l.isEmpty()) continue;
        ++i; if (!QJsonDocument::fromJson(l).isObject()) throw Unsafe{QString("AUDIT.jsonl line %1 is corrupt; run --verify-chain and investigate").arg(i)};
        lines << l;
    }
    return lines;
}
Fd lockAudit(int handoffFd) {
    Fd fd = openFile(handoffFd, Audit, O_RDWR | O_APPEND | O_CREAT);
    if (::flock(fd.fd, LOCK_EX) != 0 || ::fchmod(fd.fd, 0600) != 0) throw Unsafe{"cannot lock AUDIT.jsonl: " + sysError()};
    return fd;
}
void appendRecord(int auditFd, const QList<QByteArray> &lines, QJsonObject record) {
    record.insert("prev_hash", lines.isEmpty() ? HandoffStore::Genesis : sha256(lines.last()));
    writeAll(auditFd, QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n');
}
struct Released { QString sha256, to, from; };
std::map<QString, Released> releasedMap(const QList<QByteArray> &lines) {
    std::map<QString, Released> out;
    for (const QByteArray &l : lines) {
        const QJsonObject r = QJsonDocument::fromJson(l).object();
        if (r.value("verdict").toString() == "RELEASE") out[r.value("output").toString()] = {r.value("output_sha256").toString(), r.value("to").toString(), r.value("from").toString()};
    }
    return out;
}
// Anything in handoff/ that is not an audited release or one of the two bookkeeping files is stray.
bool stray(const QString &name, const std::map<QString, Released> &released) { return !released.count(name) && name != Audit && name != Manifest; }
void refuseBroken(int handoffFd, const QList<QByteArray> &lines) {
    const auto released = releasedMap(lines); const QStringList names = listDir(handoffFd);
    for (const QString &n : names) if (n.startsWith(".tmp-")) throw Unsafe{"leftover temp file " + n + " from a crashed release; run --verify-chain, investigate and remove it by hand"};
    for (const auto &[n, r] : released) if (!names.contains(n)) throw Unsafe{"released file " + n + " is missing (crashed release?); run --verify-chain and investigate"};
    for (const QString &n : names) if (stray(n, released)) throw Unsafe{"stray file " + n + " has no audit record; run --verify-chain and investigate"};
}
QSet<QString> manifestHashes(int handoffFd) {
    Fd fd(::openat(handoffFd, Manifest, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (fd.fd < 0 && errno == ENOENT) return {};
    struct stat st {};
    if (fd.fd < 0 || ::fstat(fd.fd, &st) != 0 || !S_ISREG(st.st_mode)) throw Unsafe{"refusing symlink or non-file: MANIFEST.txt"};
    QSet<QString> out;
    for (const QByteArray &l : readAll(fd.fd, 64 * 1024 * 1024).split('\n')) { const auto p = l.simplified().split(' '); if (p.size() >= 2) out << QString::fromUtf8(p[1]); }
    return out;
}
using Hit = HandoffCheck::Hit;
QString text(const QByteArray &data) { return QString::fromUtf8(data); }
// Agent id and inputs cross-checked between flags, headers and the audit chain (oracle provenance()).
QList<Hit> provenance(const QString &body, const std::map<QString, Released> &released, const QString &src, const QString &agentFlag, const QStringList &inputFlag, QJsonValue *agent, QStringList *names) {
    QList<Hit> reasons;
    auto resolve = [&](const QString &name, const QString &digest = {}) {
        const auto it = released.find(name);
        // recon is the one exception: it cites the spec->impl release that authorises the removal.
        const QString wantFrom = src == "recon" ? "spec" : QString(), wantTo = src == "recon" ? "impl" : src;
        if (it == released.end() || it->second.to != wantTo || (!wantFrom.isEmpty() && it->second.from != wantFrom) || (!digest.isEmpty() && digest.toLower() != it->second.sha256)) {
            reasons << Hit{0, "P2", "input is not an earlier entry released to this stage (name and sha256 must match a release whose 'to' is --from; recon must cite a spec->impl release)"};
            return false;
        }
        return true;
    };
    const auto hAgent = HandoffCheck::parseHeader(body, "Agent");
    if (!agentFlag.isEmpty() && hAgent && !hAgent->isEmpty() && agentFlag != *hAgent) reasons << Hit{0, "P1", "--agent-id disagrees with the Agent: header"};
    *agent = !agentFlag.isEmpty() ? QJsonValue(agentFlag) : hAgent ? QJsonValue(*hAgent) : QJsonValue();
    if ((src == "impl" || src == "validate" || src == "recon") && agent->toString().isEmpty()) reasons << Hit{0, "P3", "no agent id (--agent-id or Agent: header) for stage " + src};
    QStringList flagNames;
    for (const QString &n : inputFlag) if (!n.trimmed().isEmpty()) flagNames << n.trimmed();
    for (const QString &n : flagNames) resolve(n);
    *names = flagNames;
    const auto hRaw = HandoffCheck::parseHeader(body, "Inputs");
    static const QRegularExpression entryRx("^(\\d\\d-[a-z]+-to-[a-z]+\\.md)\\s+([0-9a-fA-F]{64})\\z");
    if (hRaw && src != "research") {
        QStringList hNames;
        for (const QString &raw : hRaw->split(QRegularExpression("[;,]"))) {
            const QString e = raw.trimmed();
            if (e.isEmpty()) continue;
            const auto m = entryRx.match(e);
            if (!m.hasMatch()) reasons << Hit{0, "P2", "Inputs: entry not in the form '<NN-from-to-to.md> <sha256>'"};
            else if (resolve(m.captured(1), m.captured(2))) hNames << m.captured(1);
        }
        if (!flagNames.isEmpty() && QSet<QString>(flagNames.begin(), flagNames.end()) != QSet<QString>(hNames.begin(), hNames.end())) reasons << Hit{0, "P1", "--inputs disagrees with the Inputs: header"};
        if (names->isEmpty()) *names = hNames;
    } else if (hRaw && names->isEmpty()) *names = HandoffCheck::parseList(body, "Inputs");
    if (src == "recon" && names->isEmpty()) reasons << Hit{0, "P2", "recon must cite the authorising spec->impl release in Inputs:"};
    if (src == "recon") reasons += HandoffCheck::reconFormat(body);
    return reasons;
}
QString nowUtc() { return QDateTime::currentDateTimeUtc().toString("yyyy-MM-ddTHH:mm:ss") + "+00:00"; }
QJsonArray array(const QStringList &l) { return QJsonArray::fromStringList(l); }
}
bool HandoffStore::allowedTransition(const QString &from, const QString &to) {
    static const QSet<QString> allowed{"research>spec", "spec>impl", "impl>validate", "spec>validate", "impl>spec", "validate>impl", "validate>spec", "recon>impl"};
    return allowed.contains(from + '>' + to);
}
HandoffStore::Result HandoffStore::submit(const QString &runRoot, const QString &from, const QString &to, const QString &file, const QString &agentId, const QStringList &inputs) {
    Result result;
    try {
        if (!Stages.contains(from) || !Stages.contains(to)) throw Unsafe{"unknown stage"};
        const QString root = cleanRoot(runRoot);
        const Fd rootFd = openRunRoot(root);
        const QByteArray data = readInput(root, from, file);
        const Fd hand = openHandoff(rootFd.fd, root, true);
        if (!allowedTransition(from, to)) result.reasons << Hit{0, "X1", "transition " + from + "->" + to + " not allowed"};
        if (data.contains('\0')) result.reasons << Hit{0, "R9", "binary content (NUL byte)"};
        QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
        const QString body = utf8.decode(data);
        if (utf8.hasError()) result.reasons << Hit{0, "R9", "input is not valid UTF-8"};
        result.reasons += HandoffCheck::checkTextStrict(body);
        const QString inHash = sha256(data);
        for (const QString &source : HandoffCheck::parseSources(body))
            if (std::any_of(source.begin(), source.end(), [](QChar c) { return c.category() == QChar::Other_Control; })) { result.reasons << Hit{0, "R9", "control character in the Sources header"}; break; } // would split a MANIFEST line
        const Fd audit = lockAudit(hand.fd);
        if (manifestHashes(hand.fd).contains(inHash)) result.reasons << Hit{0, "D1", "identical content is already released (re-release of a copy)"};
        const QList<QByteArray> lines = readRecords(audit.fd);
        refuseBroken(hand.fd, lines);
        const auto released = releasedMap(lines);
        QJsonValue agent; QStringList names;
        result.reasons += provenance(body, released, from, agentId, inputs, &agent, &names);
        QStringList hashes;
        for (const QString &n : names) if (released.count(n)) hashes << released.at(n).sha256;
        QJsonObject rec{{"time", nowUtc()}, {"from", from}, {"to", to}, {"author", from}, {"agent", agent}, {"inputs", array(names)}, {"input_hashes", array(hashes)},
                        {"input_sha256", inHash}, {"output_sha256", QJsonValue()}, {"sources", array(HandoffCheck::parseSources(body))}, {"report_to_compliance", false}};
        if (from == "recon") { rec.insert("stage_tainted_by_reading", true); rec.insert("reuse", "never"); }
        if (!result.reasons.isEmpty()) {
            QStringList ids;
            for (const Hit &h : result.reasons) {
                ids << (h.inner.isEmpty() ? h.rule : h.rule + '/' + h.inner);
                if (HandoffCheck::isComplianceRule(h)) { result.reportToCompliance = true; result.complianceRules << (h.inner.isEmpty() ? h.rule : h.inner); }
            }
            ids.removeDuplicates(); ids.sort(); result.complianceRules.removeDuplicates(); result.complianceRules.sort();
            rec.insert("verdict", "REJECT"); rec.insert("rules", array(ids)); rec.insert("output", QJsonValue()); rec.insert("report_to_compliance", result.reportToCompliance);
            appendRecord(audit.fd, lines, rec);
            result.status = Status::Rejected;
            return result;
        }
        // Release: temp file -> record + manifest line -> hard link, all under the audit lock. Crash-evident, not atomic.
        int n = 1;
        for (const QByteArray &l : lines) n += QJsonDocument::fromJson(l).object().value("verdict").toString() == "RELEASE";
        if (n > 99) throw Unsafe{"handoff sequence exhausted (99)"};
        const QString name = QString("%1-%2-to-%3.md").arg(n, 2, 10, QChar('0')).arg(from, to), tmp = ".tmp-" + name;
        const Fd manifest = openFile(hand.fd, Manifest, O_WRONLY | O_APPEND | O_CREAT);
        struct stat mst {};
        if (::fstat(manifest.fd, &mst) != 0 || !S_ISREG(mst.st_mode)) throw Unsafe{"MANIFEST.txt is not a regular file"};
        {
            const Fd t = openFile(hand.fd, local(tmp).constData(), O_WRONLY | O_CREAT | O_EXCL);
            writeAll(t.fd, data);
            if (::fchmod(t.fd, 0600) != 0 || ::fsync(t.fd) != 0) throw Unsafe{"cannot finish " + tmp};
        }
        rec.insert("verdict", "RELEASE"); rec.insert("rules", QJsonArray()); rec.insert("output_sha256", inHash); rec.insert("output", name);
        try {
            appendRecord(audit.fd, lines, rec);
            writeAll(manifest.fd, (name + ' ' + inHash + ' ' + from + ' ' + HandoffCheck::parseSources(body).join(';') + '\n').toUtf8());
            if (::linkat(hand.fd, local(tmp).constData(), hand.fd, local(name).constData(), 0) != 0) throw Unsafe{"cannot link " + name + ": " + sysError()};
        } catch (...) { ::unlinkat(hand.fd, local(tmp).constData(), 0); throw; }
        ::unlinkat(hand.fd, local(tmp).constData(), 0);
        result.status = Status::Released; result.released = name; result.sha256 = inHash;
    } catch (const Unsafe &e) {
        result.status = Status::Unsafe; result.error = e.message;
        // A store failure must not drop a compliance report for hits already found.
        for (const Hit &h : result.reasons) if (HandoffCheck::isComplianceRule(h)) { result.reportToCompliance = true; result.complianceRules << (h.inner.isEmpty() ? h.rule : h.inner); }
        result.complianceRules.removeDuplicates(); result.complianceRules.sort();
    }
    return result;
}
QList<HandoffStore::Release> HandoffStore::releases(const QString &runRoot, QString *error) {
    QList<Release> out;
    try {
        const QString root = cleanRoot(runRoot);
        const Fd rootFd = openRunRoot(root), hand = openHandoff(rootFd.fd, root, false);
        const Fd audit = openFile(hand.fd, Audit, O_RDONLY);
        for (const QByteArray &l : readRecords(audit.fd)) {
            const QJsonObject r = QJsonDocument::fromJson(l).object();
            if (r.value("verdict").toString() == "RELEASE") out << Release{r.value("output").toString(), r.value("output_sha256").toString(), r.value("from").toString(), r.value("to").toString()};
        }
    } catch (const Unsafe &e) { if (error) *error = e.message; return {}; }
    return out;
}
QString HandoffStore::appendEvent(const QString &runRoot, const QString &verdict, QJsonObject fields) {
    try {
        const QString root = cleanRoot(runRoot);
        const Fd rootFd = openRunRoot(root), hand = openHandoff(rootFd.fd, root, true), audit = lockAudit(hand.fd);
        const QList<QByteArray> lines = readRecords(audit.fd);
        fields.insert("time", nowUtc()); fields.insert("verdict", verdict);
        appendRecord(audit.fd, lines, fields);
    } catch (const Unsafe &e) { return e.message; }
    return {};
}
HandoffStore::Chain HandoffStore::verifyChain(const QString &runRoot, bool check) {
    Chain chain;
    try {
        const QString root = cleanRoot(runRoot);
        struct stat st {};
        if (::lstat(local(root).constData(), &st) == 0 && S_ISLNK(st.st_mode)) throw Unsafe{"run root is a symlink: " + runRoot};
        const Fd rootFd = openDir(AT_FDCWD, root, root);
        Fd hand(::openat(rootFd.fd, "handoff", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (hand.fd < 0) { chain.exitCode = 1; chain.problems << "handoff dir missing or a symlink"; return chain; }
        Fd auditFd(::openat(hand.fd, Audit, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
        if (auditFd.fd < 0 || ::fstat(auditFd.fd, &st) != 0 || !S_ISREG(st.st_mode)) { chain.exitCode = 1; chain.problems << "AUDIT.jsonl missing or a symlink"; return chain; }
        QList<QByteArray> lines;
        for (const QByteArray &l : readAll(auditFd.fd, std::numeric_limits<qint64>::max()).split('\n')) if (!l.isEmpty()) lines << l;
        QString prev = Genesis;
        std::map<QString, Released> released;
        for (int i = 0; check && i < lines.size(); ++i) {
            const QString where = QString("audit line %1: ").arg(i + 1);
            const QJsonDocument doc = QJsonDocument::fromJson(lines[i]);
            if (!doc.isObject()) { chain.problems << where + "not a JSON object"; break; }
            const QJsonObject rec = doc.object();
            if (rec.contains("report_to_compliance") && !rec.value("report_to_compliance").isBool()) chain.problems << where + "report_to_compliance is not a JSON boolean";
            if (rec.value("prev_hash").toString() != prev || !rec.value("prev_hash").isString()) chain.problems << where + "prev_hash mismatch (record removed or altered before it)";
            prev = sha256(lines[i]);
            if (rec.value("verdict").toString() != "RELEASE") continue;
            const QString name = rec.value("output").toString();
            Fd body(name.contains('/') || name.isEmpty() ? -1 : ::openat(hand.fd, local(name).constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
            if (body.fd < 0 || ::fstat(body.fd, &st) != 0 || !S_ISREG(st.st_mode)) chain.problems << where + "released file " + name + " missing";
            else {
                const QByteArray content = readAll(body.fd, std::numeric_limits<qint64>::max());
                if (sha256(content) != rec.value("output_sha256").toString()) chain.problems << where + "released file " + name + " was modified";
                const QString from = rec.value("from").toString();
                static const QRegularExpression hash64("(?<![\\p{L}\\p{N}_])[0-9a-f]{64}(?![\\p{L}\\p{N}_])", QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption); // python's \\b
                if (from != "research")
                    for (auto it = hash64.globalMatch(HandoffCheck::parseHeader(QString::fromUtf8(content), "Inputs").value_or(QString())); it.hasNext();) {
                        const QString h = it.next().captured();
                        bool found = false;
                        for (const auto &[n, r] : released) found = found || (r.sha256 == h.toLower() && (from == "recon" ? r.to == "impl" && r.from == "spec" : r.to == from));
                        if (!found) chain.problems << name + ": Inputs hash " + h.left(12) + "... matches no earlier released entry";
                    }
            }
            released[name] = {rec.value("output_sha256").toString(), rec.value("to").toString(), rec.value("from").toString()};
        }
        if (check) {
            for (const QString &n : listDir(hand.fd)) {
                if (n.startsWith(".tmp-")) chain.problems << n + ": leftover temp file from a crashed release";
                else if (stray(n, released)) chain.problems << n + ": stray file with no audit record";
            }
            std::map<QString, QString> manifest, expected;
            Fd m(::openat(hand.fd, Manifest, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
            if (m.fd < 0 && errno == ELOOP) chain.problems << "MANIFEST.txt is a symlink";
            else if (m.fd >= 0 && ::fstat(m.fd, &st) == 0 && S_ISREG(st.st_mode))
                for (const QByteArray &l : readAll(m.fd, std::numeric_limits<qint64>::max()).split('\n')) { const auto p = l.simplified().split(' '); if (p.size() >= 2 && !p[0].isEmpty()) manifest[QString::fromUtf8(p[0])] = QString::fromUtf8(p[1]); }
            for (const auto &[n, r] : released) expected[n] = r.sha256;
            if (manifest != expected) chain.problems << "MANIFEST.txt does not match released records in AUDIT.jsonl";
            if (!chain.problems.isEmpty()) { chain.exitCode = 1; return chain; }
        }
        prev = lines.isEmpty() ? Genesis : sha256(lines.last());
        int tainted = 0, flagged = 0, recon = 0;
        static const QRegularExpression taintDir("^(?:research|spec|impl|validate|recon)\\.tainted$"); // compliance ruling: recon counts
        for (const QString &n : listDir(rootFd.fd)) {
            if (!taintDir.match(n).hasMatch() || ::fstatat(rootFd.fd, local(n).constData(), &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(st.st_mode)) continue;
            const Fd t = openDir(rootFd.fd, n, n); int k = 0;
            for (const QString &c : listDir(t.fd)) k += ::fstatat(t.fd, local(c).constData(), &st, 0) == 0 && S_ISDIR(st.st_mode);
            tainted += k;
            if (!k) chain.warnings << "warning: " + n + " has no <agent-id> subdirectory and counts 0; taints belong in " + n + "/<agent-id>/";
        }
        for (const QByteArray &l : lines) {
            const QJsonObject r = QJsonDocument::fromJson(l).object();
            flagged += r.value("report_to_compliance").toBool();
            recon += r.value("verdict").toString() == "RELEASE" && r.value("from").toString() == "recon";
        }
        chain.summary = QString("records=%1 last_hash=%2 tainted_stages=%3 compliance_reports=%4 recon_releases=%5").arg(lines.size()).arg(prev).arg(tainted).arg(flagged).arg(recon);
        chain.exitCode = 0;
    } catch (const Unsafe &e) { chain.exitCode = 2; chain.problems << "error: " + e.message; }
    return chain;
}
