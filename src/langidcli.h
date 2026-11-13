// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QStringList>
#include <QTextStream>

// `darling-launcher langid-import --volume DIR --prefix DIR`: the headless form of the "Import language data" button.
// args are the arguments after "langid-import". It calls LauncherCore::importLangidData, so every source and
// destination check of the button applies unchanged. Prints "imported N files" or "already present" on out.
// Exit codes: 0 ok, 1 refused or failed (one line on err), 2 usage. `--help` prints the usage and returns 0.
int langidImportMain(const QStringList &args, QTextStream &out, QTextStream &err);
