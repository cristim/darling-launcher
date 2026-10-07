// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QList>
#include <QStringList>
struct SourceMount { QString path, device, filesystem; bool readOnly = false; };
struct SourceCandidate {
    QString mountPoint, root, device;
    bool readable = false, apps = false, libraries = false, readOnly = false;
    bool usable() const { return readable && (apps || libraries); }
    QString description() const;
};
namespace LauncherSources {
QList<SourceMount> parseMountInfo(const QByteArray &data);
QList<SourceMount> mounts();
QList<SourceCandidate> candidates(const QList<SourceMount> &mounts, const QString &device = {});
QString automaticSource(const QList<SourceCandidate> &candidates, const QString &current);
}
