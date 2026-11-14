// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QStringList>
#include <QTextStream>

// `darling-launcher frameworks-import --volume DIR --prefix DIR [--only NAME[,NAME...]] [--dry-run] [--yes]`: the headless form of the
// "Import framework resources" button. args are the arguments after "frameworks-import". --dry-run prints the plan
// and the size estimate and copies nothing; without --yes the plan is printed and nothing is copied either, because
// the import needs a confirmation (the GUI asks in a dialog). --only limits the import to the named framework bundles
// (base names, nested frameworks included); a name that matches no bundle fails with exit 1.
// Exit codes: 0 done or dry run, 1 refused or failed (one line on err), 2 usage or missing confirmation.
int frameworksImportMain(const QStringList &args, QTextStream &out, QTextStream &err);
