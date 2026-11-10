// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QStringList>
#include <QTextStream>

// `darling-launcher dyld-cache <verb> ...`. args are the arguments after "dyld-cache".
//   copy    --source DIR [--store-root R] [--arch arm64e|arm64] [--window-mib N] [--throttle-ms N] [--margin-mib N]
//           copies the cache files into the private store; progress lines on err; SIGINT stops cleanly and keeps the partial copy
//   status  [--source DIR] [--store-root R] [--arch A]   TSV per store: id, state, files, bytes, copiedAt[, current|stale]; with --source also a source line
//   verify  [--store-root R] [--store ID]                 re-checks sizes, header UUIDs and sha256 of the local store(s)
//   prefix  --prefix P (--enable [--store ID] [--lib NAME]... | --disable) [--store-root R]
//           writes <P>/.darling-launcher/dyld-cache.json and the policy file; only validated libraries are accepted
// The store location defaults to the launcher setting. Neither it nor a resolved source path is ever printed.
// Exit codes: 0 ok, 1 refused or failed (one line on err), 2 usage, 130 copy cancelled by SIGINT (partial copy kept).
// `dyld-cache --help` prints the usage and returns 0.
int dyldCacheMain(const QStringList &args, QTextStream &out, QTextStream &err);
