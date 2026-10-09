// SPDX-License-Identifier: GPL-3.0-or-later
#include "trace.h"
#include "tracevars.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
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
    int parent = openDirectory(prefix + "/.darling-launcher", false, error);
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
// Raw words are shown as hex text only; anything else is dropped rather than interpreted.
QString hexWord(const QJsonValue &value) {
    if (value.isString()) { static const QRegularExpression pattern("^(0[xX])?[0-9a-fA-F]{1,16}$"); return pattern.match(value.toString()).hasMatch() ? value.toString() : QString(); }
    if (value.isDouble() && value.toDouble() >= 0 && value.toDouble() < 9007199254740992.0) return "0x" + QString::number(static_cast<quint64>(value.toDouble()), 16);
    return {};
}
}

QStringList LauncherTrace::modes() { return {TraceVars::ModeLog, TraceVars::ModeTrap, TraceVars::ModeContinue}; }
bool LauncherTrace::validMode(const QString &mode) { return modes().contains(mode); }
QString LauncherTrace::traceDirectory(const QString &prefix) { return prefix + "/.darling-launcher/" + TraceVars::TraceDirName; }

bool LauncherTrace::ensurePrivateDirectory(const QString &path, QString *error, bool requirePrivate) {
    const int fd = openDirectory(path, requirePrivate, error);
    if (fd >= 0) ::close(fd);
    return fd >= 0;
}

bool LauncherTrace::prepareLog(const QString &prefix, const QString &app, QString *logPath, QString *error) {
    QString safe = app; safe.replace(QRegularExpression("[^A-Za-z0-9._-]"), "_"); if (safe.isEmpty() || safe.startsWith('.')) safe.prepend('_');
    int fd = -1;
    if (!createPrivateFile(prefix, utcName(safe.left(64), ".jsonl"), logPath, &fd, error)) return false;
    ::close(fd);
    return true;
}

bool LauncherTrace::applyTrace(QProcessEnvironment &env, const QString &prefix, const QString &app, bool enabled, const QString &mode, QString *logPath, QString *error) {
    for (const auto &name : env.keys()) if (name.startsWith(TraceVars::Mode)) env.remove(name);
    if (!enabled) return true;
    if (!validMode(mode)) return fail(error, "Unknown trace mode: " + mode);
    QString path;
    if (!prepareLog(prefix, app, &path, error)) return false;
    env.insert(TraceVars::Mode, mode); env.insert(TraceVars::Log, path);
    if (logPath) *logPath = path;
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
    QMap<QPair<QString, QString>, Row> rows;
    for (const QByteArray &line : data.split('\n')) {
        if (line.trimmed().isEmpty()) continue;
        if (line.size() > MaxLine) { ++summary.skipped; continue; }
        QJsonParseError parseError; const auto object = QJsonDocument::fromJson(line, &parseError).object();
        const QString symbol = clean(object.value("symbol").toString()), image = clean(object.value("image").toString());
        if (parseError.error != QJsonParseError::NoError || symbol.isEmpty()) { ++summary.skipped; continue; }
        const QPair<QString, QString> key{image, symbol};
        if (!rows.contains(key) && rows.size() >= MaxRows) { ++summary.skipped; summary.truncated = true; continue; }
        auto &row = rows[key];
        if (row.symbol.isEmpty()) { row.image = image; row.symbol = symbol; }
        const double count = object.value("count").toDouble(1);
        const qint64 add = count >= 1 && count <= MaxLineCount ? static_cast<qint64>(count) : 1;
        row.count = qMin<qint64>(MaxCount, row.count + add);
        if (row.details.isEmpty()) {
            QStringList words; for (const auto &value : object.value("regs").toArray()) { const QString word = hexWord(value); if (!word.isEmpty()) words << word; }
            row.details = words.mid(0, 16).join(' ');
        }
    }
    summary.rows = rows.values();
    return summary;
}

QString LauncherTrace::exportSummary(const Summary &summary) {
    QString text = "image\tsymbol\tcount\n";
    for (const auto &row : summary.rows) text += clean(row.image) + '\t' + clean(row.symbol) + '\t' + QString::number(row.count) + '\n';
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
