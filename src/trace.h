// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QList>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <unistd.h>

namespace LauncherTrace {
// One JSONL record from the producer (dyld's MissingSymbolTrace). "missing" records carry
// symbol/library/from; "truncated" and "scan_skipped" records carry no symbol and may carry
// library/from/reason. There is no count: the producer writes one record per unresolved import.
struct Row { QString symbol, library, from, kind, reason; };
inline constexpr int MaxText = 256, MaxLine = 64 * 1024, MaxRows = 10000;
struct Summary { QList<Row> rows; int skipped = 0; bool truncated = false, unreadable = false; };

// True when darling would run with extra privileges (setuid/setgid binary or an elevated launcher); tracing is refused then.
bool elevated(const QString &darling, uid_t euid = ::geteuid());
QString traceDirectory(const QString &prefix);
// Creates the directory if absent; refuses a symlink, a foreign owner and (when requirePrivate) group/other access.
bool ensurePrivateDirectory(const QString &path, QString *error, bool requirePrivate = true);
// Sets the trace variables only for a traced launch, and always strips any inherited trace variables first, so a
// parent shell cannot force tracing on an untraced app. The log variable names the trace directory as an absolute
// guest path; dyld creates the per-process log file inside it.
bool applyTrace(QProcessEnvironment &env, const QString &prefix, bool enabled, const QString &darling, QString *error);
Summary parse(const QString &path, qint64 maxBytes = 8 << 20);
QString exportSummary(const Summary &summary);
// Writes the summary as a new 0600 file in the private trace directory.
bool writeSummary(const QString &prefix, const Summary &summary, QString *path, QString *error);
}
