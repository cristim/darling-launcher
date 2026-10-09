// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QList>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

namespace LauncherTrace {
struct Row { QString image, symbol, details; qint64 count = 0; };
struct Summary { QList<Row> rows; int skipped = 0; bool truncated = false; };

QStringList modes();
bool validMode(const QString &mode);
QString traceDirectory(const QString &prefix);
// Creates the directory if absent; refuses a symlink, a foreign owner and (when requirePrivate) group/other access.
bool ensurePrivateDirectory(const QString &path, QString *error, bool requirePrivate = true);
// Creates the per-prefix directory (0700, user-owned, never a symlink) and an empty 0600 log for this launch.
bool prepareLog(const QString &prefix, const QString &app, QString *logPath, QString *error);
// Adds the trace variables to a launched app's environment only; with trace off the environment is untouched.
bool applyTrace(QProcessEnvironment &env, const QString &prefix, const QString &app, bool enabled, const QString &mode, QString *logPath, QString *error);
Summary parse(const QString &path, qint64 maxBytes = 8 << 20);
QString exportSummary(const Summary &summary);
}
