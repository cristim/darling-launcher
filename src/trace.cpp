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
}
bool LauncherTrace::ensurePrivateDirectory(const QString &path, QString *error, bool requirePrivate) {
    const QByteArray native = QFile::encodeName(path);
    if (::mkdir(native.constData(), 0700) != 0 && errno != EEXIST) return fail(error, "Cannot create " + path);
    struct stat info;
    if (::lstat(native.constData(), &info) != 0 || !S_ISDIR(info.st_mode)) return fail(error, path + " is not a plain directory");
    if (info.st_uid != ::geteuid()) return fail(error, path + " is not owned by you");
    if (requirePrivate && (info.st_mode & 077)) return fail(error, path + " is accessible to other users");
    return true;
}
namespace {
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

bool LauncherTrace::prepareLog(const QString &prefix, const QString &app, QString *logPath, QString *error) {
    if (!ensurePrivateDirectory(prefix + "/.darling-launcher", error, false) || !ensurePrivateDirectory(traceDirectory(prefix), error)) return false;
    QString safe = app; safe.replace(QRegularExpression("[^A-Za-z0-9._-]"), "_"); if (safe.isEmpty() || safe.startsWith('.')) safe.prepend('_');
    const QString path = traceDirectory(prefix) + '/' + safe.left(64) + '-' + QDateTime::currentDateTimeUtc().toString("yyyyMMdd'T'HHmmss'Z'") + ".jsonl";
    const int fd = ::open(QFile::encodeName(path).constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return fail(error, "Cannot create trace log " + path);
    ::close(fd);
    *logPath = path;
    return true;
}

bool LauncherTrace::applyTrace(QProcessEnvironment &env, const QString &prefix, const QString &app, bool enabled, const QString &mode, QString *logPath, QString *error) {
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
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return summary;
    QByteArray data = file.read(maxBytes);
    summary.truncated = !file.atEnd();
    if (summary.truncated) { const int cut = data.lastIndexOf('\n'); data.truncate(cut < 0 ? 0 : cut); }
    QMap<QPair<QString, QString>, Row> rows;
    for (const QByteArray &line : data.split('\n')) {
        if (line.trimmed().isEmpty()) continue;
        QJsonParseError parseError; const auto object = QJsonDocument::fromJson(line, &parseError).object();
        const QString symbol = object.value("symbol").toString();
        if (parseError.error != QJsonParseError::NoError || symbol.isEmpty()) { ++summary.skipped; continue; }
        const QString image = object.value("image").toString();
        auto &row = rows[{image, symbol}];
        if (row.symbol.isEmpty()) { row.image = image; row.symbol = symbol; }
        const double count = object.value("count").toDouble(1);
        row.count += count >= 1 && count < 1e15 ? static_cast<qint64>(count) : 1;
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
    for (const auto &row : summary.rows) text += row.image + '\t' + row.symbol + '\t' + QString::number(row.count) + '\n';
    return text;
}
