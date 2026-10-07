// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QStringList>
namespace LauncherDiscovery {
QStringList roots();
QString defaultVolume(const QStringList &mounts, const QString &current);
QStringList cloneArguments(const QString &repository, const QString &destination);
QStringList scripts(const QStringList &roots);
QStringList sources(const QStringList &roots);
QString newWorkspace(const QString &base);
}
