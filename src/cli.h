// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QStringList>
#include <functional>
class QCoreApplication;

namespace LauncherCli {
enum class EntryKind { Gui, Cli, Error };
struct Entry { EntryKind kind = EntryKind::Cli; QStringList arguments, reasons; QString backend, error; bool mountAll = false; };
Entry selectEntry(QStringList arguments, const QProcessEnvironment &environment, const std::function<QString(QStringList *)> &probe);
void prepareGuiEnvironment(const Entry &entry);
QString helpText();
QJsonObject dispatch(const QStringList &arguments);
int run(QCoreApplication &application, const QStringList &arguments, const QString &error = {});
}
