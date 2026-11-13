// SPDX-License-Identifier: GPL-3.0-or-later
#include "langidcli.h"
#include "core.h"

int langidImportMain(const QStringList &args, QTextStream &out, QTextStream &err) {
    const QString usage = "usage: darling-launcher langid-import --volume DIR --prefix DIR";
    if (args == QStringList{"--help"}) { out << usage << Qt::endl; return 0; }
    QString volume, prefix;
    for (int i = 0; i < args.size(); i += 2) {
        QString *target = args[i] == "--volume" ? &volume : args[i] == "--prefix" ? &prefix : nullptr;
        if (!target || i + 1 >= args.size() || args[i + 1].isEmpty() || !target->isEmpty()) { err << usage << Qt::endl; return 2; }
        *target = args[i + 1];
    }
    if (volume.isEmpty() || prefix.isEmpty()) { err << usage << Qt::endl; return 2; }
    QStringList copied; QString error;
    if (!LauncherCore::importLangidData(volume, prefix, &copied, &error)) { err << error << Qt::endl; return 1; }
    out << (copied.isEmpty() ? QString("already present") : QString("imported %1 files").arg(copied.size())) << Qt::endl;
    return 0;
}
