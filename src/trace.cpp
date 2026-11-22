// SPDX-License-Identifier: GPL-3.0-or-later
#include "trace.h"
#include "tracevars.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
const QStringList KnownKinds{"missing", "truncated", "scan_skipped"};
bool fail(QString *error, const QString &message) { if (error) *error = message; return false; }
// Opens without following a final symlink and checks the opened descriptor, so later openat() calls cannot be redirected.
int openDirectory(const QString &path, bool requirePrivate, QString *error) {
    const QByteArray native = QFile::encodeName(path);
    if (::mkdir(native.constData(), 0700) != 0 && errno != EEXIST) { fail(error, "Cannot create " + path); return -1; }
    const int fd = ::open(native.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { fail(error, path + " is not a plain directory"); return -1; }
    struct stat info;
    if (::fstat(fd, &info) != 0 || info.st_uid != ::geteuid()) { ::close(fd); fail(error, path + " is not owned by you"); return -1; }
    if (requirePrivate && (info.st_mode & 077)) { ::close(fd); fail(error, path + " is accessible to other users"); return -1; }
    return fd;
}
// Creates a new 0600 file inside the verified directory; fails if the name exists, even as a symlink.
bool createPrivateFile(const QString &prefix, const QString &name, QString *path, int *fdOut, QString *error) {
    int parent = openDirectory(prefix + "/" + TraceVars::DataDirName, false, error);
    if (parent < 0) return false;
    ::close(parent);
    const int dir = openDirectory(LauncherTrace::traceDirectory(prefix), true, error);
    if (dir < 0) return false;
    const int fd = ::openat(dir, QFile::encodeName(name).constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    ::close(dir);
    if (fd < 0) return fail(error, "Cannot create " + name);
    *path = LauncherTrace::traceDirectory(prefix) + '/' + name; *fdOut = fd;
    return true;
}
QString utcName(const QString &stem, const char *extension) { return stem + '-' + QDateTime::currentDateTimeUtc().toString("yyyyMMdd'T'HHmmss'Z'") + extension; }
// Log content is untrusted: control and format characters are replaced so it cannot forge lines or terminal escapes.
QString clean(const QString &text) {
    QString out = text.left(LauncherTrace::MaxText);
    for (QChar &c : out) if (!c.isPrint() || c.category() == QChar::Other_Format || c.category() == QChar::Separator_Line || c.category() == QChar::Separator_Paragraph) c = '?';
    return out;
}
}

bool LauncherTrace::elevated(const QString &darling, uid_t euid) {
    struct stat info;
    return euid == 0 || ::stat(QFile::encodeName(darling).constData(), &info) != 0 || (info.st_mode & (S_ISUID | S_ISGID));
}
QString LauncherTrace::traceDirectory(const QString &prefix) { return prefix + "/" + TraceVars::DataDirName + "/" + TraceVars::TraceDirName; }

bool LauncherTrace::ensurePrivateDirectory(const QString &path, QString *error, bool requirePrivate) {
    const int fd = openDirectory(path, requirePrivate, error);
    if (fd >= 0) ::close(fd);
    return fd >= 0;
}

bool LauncherTrace::applyTrace(QProcessEnvironment &env, const QString &prefix, bool enabled, const QString &darling, QString *error) {
    for (const auto &name : env.keys()) if (name.startsWith(TraceVars::Mode)) env.remove(name);
    if (!enabled || elevated(darling)) return true;
    if (!ensurePrivateDirectory(prefix + "/" + TraceVars::DataDirName, error, false)) return false;
    if (!ensurePrivateDirectory(traceDirectory(prefix), error, true)) return false;
    env.insert(TraceVars::Mode, TraceVars::ModeLog); env.insert(TraceVars::Log, TraceVars::TraceDirGuestPath);
    return true;
}

LauncherTrace::Summary LauncherTrace::parse(const QString &path, qint64 maxBytes) {
    Summary summary;
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    struct stat info;
    QFile file;
    if (fd < 0 || ::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || !file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) { if (fd >= 0 && !file.isOpen()) ::close(fd); summary.unreadable = true; return summary; }
    QByteArray data = file.read(maxBytes);
    summary.truncated = !file.atEnd();
    if (summary.truncated) { const int cut = data.lastIndexOf('\n'); data.truncate(cut < 0 ? 0 : cut); }
    for (const QByteArray &line : data.split('\n')) {
        if (line.trimmed().isEmpty()) continue;
        if (line.size() > MaxLine) { ++summary.skipped; continue; }
        QJsonParseError parseError; const QJsonObject object = QJsonDocument::fromJson(line, &parseError).object();
        const QString kind = object.value("kind").toString();
        const QJsonValue version = object.value("v");
        bool validFields = true;
        for (const char *field : {"sym", "lib", "from", "arch", "reason"})
            if (object.contains(field) && !object.value(field).isString()) validFields = false;
        if (parseError.error != QJsonParseError::NoError || !version.isDouble() || version.toDouble() != 1 || !KnownKinds.contains(kind) || !validFields) { ++summary.skipped; continue; }
        Row row; row.kind = kind;
        if (kind == QStringLiteral("missing")) { row.symbol = clean(object.value("sym").toString()); if (row.symbol.isEmpty()) { ++summary.skipped; continue; } }
        row.library = clean(object.value("lib").toString()); row.from = clean(object.value("from").toString()); row.reason = clean(object.value("reason").toString());
        if (summary.rows.size() >= MaxRows) { ++summary.skipped; summary.truncated = true; continue; }
        summary.rows.append(row);
    }
    return summary;
}

QString LauncherTrace::exportSummary(const Summary &summary) {
    QString text = "symbol\tlibrary\treferencing_image\tkind\n";
    for (const auto &row : summary.rows) {
        const QString kind = row.reason.isEmpty() ? clean(row.kind) : clean(row.kind) + " (" + clean(row.reason) + ")";
        text += clean(row.symbol) + '\t' + clean(row.library) + '\t' + clean(row.from) + '\t' + kind + '\n';
    }
    return text;
}

bool LauncherTrace::writeSummary(const QString &prefix, const Summary &summary, QString *path, QString *error) {
    int fd = -1;
    if (!createPrivateFile(prefix, utcName("summary", ".tsv"), path, &fd, error)) return false;
    QFile out;
    if (!out.open(fd, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle)) { ::close(fd); return fail(error, "Cannot write " + *path); }
    const QByteArray bytes = exportSummary(summary).toUtf8();
    if (out.write(bytes) != bytes.size()) return fail(error, "Cannot write " + *path);
    return true;
}
