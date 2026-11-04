// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>
#include <QStringList>

namespace LauncherDisplay {
int probe(const QString &backend);
QString availableBackend(QStringList *reasons);
}
