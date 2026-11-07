// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QStringList>
#include <QTextStream>

// `darling-launcher libs <verb> ...`. args are the arguments after "libs".
//   list    --prefix P [--runtime-root R]              TSV: installName, status, source kind
//   import  --prefix P --runtime-root R --name N --file F [--force] [--source-kind volume|fixture]
//   enable  --prefix P --runtime-root R --name N [--force]
//   disable --prefix P --name N
//   abandon --prefix P --name N
//   master  on|off --prefix P [--runtime-root R]       on needs --runtime-root, [--force]
// Exit codes: 0 ok, 1 refused or failed (one line on err), 2 usage, 3 master off skipped files that are not ours
// (their names are on err). `libs --help` prints the usage and returns 0.
int libsMain(const QStringList &args, QTextStream &out, QTextStream &err);
