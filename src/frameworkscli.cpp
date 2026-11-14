// SPDX-License-Identifier: GPL-3.0-or-later
#include "frameworkscli.h"
#include "frameworkresources.h"

int frameworksImportMain(const QStringList &args, QTextStream &out, QTextStream &err) {
    const QString usage = "usage: darling-launcher frameworks-import --volume DIR --prefix DIR [--dry-run] [--yes]";
    if (args == QStringList{"--help"}) { out << usage << Qt::endl; return 0; }
    QString volume, prefix; bool dryRun = false, yes = false;
    for (int i = 0; i < args.size(); ++i) {
        if (args[i] == "--dry-run" && !dryRun) { dryRun = true; continue; }
        if (args[i] == "--yes" && !yes) { yes = true; continue; }
        QString *target = args[i] == "--volume" ? &volume : args[i] == "--prefix" ? &prefix : nullptr;
        if (!target || i + 1 >= args.size() || args[i + 1].isEmpty() || !target->isEmpty()) { err << usage << Qt::endl; return 2; }
        *target = args[++i];
    }
    if (volume.isEmpty() || prefix.isEmpty() || (dryRun && yes)) { err << usage << Qt::endl; return 2; }
    FrameworkResources::Plan plan; QString error;
    if (!FrameworkResources::buildPlan(volume, prefix, &plan, &error)) { err << error << Qt::endl; return 1; }
    out << FrameworkResources::summarize(plan);
    if (dryRun) { out << "Dry run: nothing was copied." << Qt::endl; return 0; }
    if (!yes) { err << "Nothing was copied. Re-run with --yes to import " << FrameworkResources::formatBytes(plan.bytes) << ", or with --dry-run to only see the plan." << Qt::endl; return 2; }
    if (plan.entries.isEmpty()) { out << "Already present: nothing to copy." << Qt::endl; return 0; }
    FrameworkResources::Result result;
    if (!FrameworkResources::applyPlan(volume, prefix, plan, &result, &error)) { err << error << Qt::endl; return 1; }
    out << QString("Imported %1 files (%2), %3 directories, %4 symlinks; %5 cloned, %6 appeared meanwhile and were left alone.")
               .arg(result.files).arg(FrameworkResources::formatBytes(result.bytes)).arg(result.directories).arg(result.symlinks)
               .arg(result.reflinked).arg(result.appeared) << Qt::endl;
    return 0;
}
