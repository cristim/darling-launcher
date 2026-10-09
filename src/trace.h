// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QList>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <unistd.h>

namespace LauncherTrace {
struct Row { QString image, symbol; qint64 count = 0; int order = 0; };
inline constexpr int MaxText = 256, MaxLine = 64 * 1024, MaxRows = 10000;
inline constexpr qint64 MaxLineCount = 1000000000, MaxCount = 1000000000000;
struct Summary { QList<Row> rows; int skipped = 0; bool truncated = false, unreadable = false; };

// True when darling would run with extra privileges (setuid/setgid binary or an elevated launcher); tracing is refused then.
bool elevated(const QString &darling, uid_t euid = ::geteuid());
QString traceDirectory(const QString &prefix);
// Creates the directory if absent; refuses a symlink, a foreign owner and (when requirePrivate) group/other access.
bool ensurePrivateDirectory(const QString &path, QString *error, bool requirePrivate = true);
// Creates the per-prefix directory (0700, user-owned, never a symlink) and an empty 0600 log for this launch.
bool prepareLog(const QString &prefix, const QString &app, QString *logPath, QString *error);
// Adds the trace variables to a launched app's environment only; with trace off the environment is untouched.
bool applyTrace(QProcessEnvironment &env, const QString &prefix, const QString &app, bool enabled, const QString &darling, QString *logPath, QString *error);
Summary parse(const QString &path, qint64 maxBytes = 8 << 20);
QString exportSummary(const Summary &summary);
// Writes the names-and-counts summary as a new 0600 file in the private trace directory.
bool writeSummary(const QString &prefix, const Summary &summary, QString *path, QString *error);
}
