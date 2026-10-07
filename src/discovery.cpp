// SPDX-License-Identifier: GPL-3.0-or-later
#include "discovery.h"
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
namespace LauncherDiscovery {
QStringList cloneArguments(const QString &repository, const QString &destination) { return {"clone", "--progress", "--", repository, destination}; }
QString defaultVolume(const QStringList &mounts, const QString &current) { return mounts.size() == 1 ? mounts.first() : current; }
QStringList roots() { return {QDir::homePath()+"/src", QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)+"/sources", QDir::tempPath()}; }
static QStringList candidates(const QStringList &roots) {
    QStringList result;
    for (const auto &root : roots) {
        result << root;
        QDir dir(root);
        for (const auto &entry : dir.entryInfoList(QDir::Dirs|QDir::NoDotAndDotDot|QDir::NoSymLinks))
            if (entry.fileName().contains("darling", Qt::CaseInsensitive)) result << entry.absoluteFilePath();
    }
    result.removeDuplicates(); return result;
}
QStringList scripts(const QStringList &roots) {
    QStringList result;
    for (const auto &path : candidates(roots)) {
        QString script = path+"/tools/all-vibedarling-pr-prefix.py";
        if (QFileInfo(script).isFile()) result << script;
    }
    return result;
}
QStringList sources(const QStringList &roots) {
    QStringList result;
    for (const auto &path : candidates(roots)) {
        if (QFileInfo(path+"/.git").exists() && QFileInfo(path+"/CMakeLists.txt").isFile() && QFileInfo(path+"/src/startup").isDir()) result << path;
    }
    return result;
}
QString newWorkspace(const QString &base) {
    QString path = base+"/darling-workspace";
    for (int i=2; QFileInfo::exists(path); ++i) path = base+"/darling-workspace-"+QString::number(i);
    return path;
}
}
