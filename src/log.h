// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>
namespace LauncherLog {
QString path();
void write(const QString &source, const QString &text);
// Like write(), but reports whether every line reached the log file.
bool writeChecked(const QString &source, const QString &text);
void installQtHandler();
}
