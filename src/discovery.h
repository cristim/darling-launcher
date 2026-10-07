// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QStringList>
#include <QList>
namespace LauncherDiscovery {
struct Runtime { QString launcher; QString installRoot; };
QList<Runtime> runtimes(const QStringList &roots, const QString &installedLauncher);
QString managedPrefix(const QString &dataRoot);
QString cleanSource(const QStringList &roots);
QString helperExecutable(const QStringList &roots, const QString &name);

QStringList roots();
QString defaultVolume(const QStringList &mounts, const QString &current);
QStringList cloneArguments(const QString &repository, const QString &destination);
QStringList scripts(const QStringList &roots);
QStringList sources(const QStringList &roots);
QString newWorkspace(const QString &base);
}
