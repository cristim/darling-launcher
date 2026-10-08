// SPDX-License-Identifier: GPL-3.0-or-later
#include "discovery.h"
#include <algorithm>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QFile>
#include <QProcess>
#include <QSettings>
namespace LauncherDiscovery {
QStringList cloneArguments(const QString &repository, const QString &destination) { return {"clone", "--progress", "--", repository, destination}; }
QStringList updateArguments(const QString &clone) { return {"-C", clone, "pull", "--ff-only", "--progress"}; }
QString defaultVolume(const QStringList &mounts, const QString &current) { return mounts.size() == 1 ? mounts.first() : current; }
QString dataRoot() { return QDir::homePath() + "/.darling-launcher"; }
QStringList roots() { return {dataRoot() + "/workspaces"}; }
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
QString helperExecutable(const QStringList &roots, const QString &name) {
    QString installed = QStandardPaths::findExecutable(name);
    if (!installed.isEmpty()) return installed;
    QStringList search;
    for (const auto &root : roots) search << root + "/apfs-fuse";
    search += candidates(roots);
    for (const auto &path : search) {
        QString helper = path + "/build/" + name;
        if (QFileInfo(helper).isExecutable()) return helper;
    }
    return {};
}
QString managedPrefix(const QString &dataRoot) { return dataRoot + "/prefixes/default"; }
QList<Runtime> runtimes(const QStringList &roots, const QString &installedLauncher) {
    QList<Runtime> result;
    auto add = [&](const QString &launcher, const QString &root) {
        if (!QFileInfo(launcher).isExecutable() || !QFileInfo(root + "/libexec/darling/private/etc").isDir()) return;
        for (const auto &item : result) if (item.launcher == launcher && item.installRoot == root) return;
        result.append({launcher, root});
    };
    if (!installedLauncher.isEmpty()) {
        QDir install(QFileInfo(installedLauncher).absolutePath()); install.cdUp();
        add(installedLauncher, install.absolutePath());
    }
    for (const auto &path : candidates(roots)) {
        add(path + "/build/src/startup/darling", path + "/image/usr/local");
        add(path + "/build/src/startup/darling", path + "/install/usr/local");
        add(path + "/build/src/startup/darling", path + "/build/image/usr/local");
    }
    std::sort(result.begin(), result.end(), [](const Runtime &a, const Runtime &b) { return QFileInfo(a.launcher).lastModified() > QFileInfo(b.launcher).lastModified(); });
    return result;
}
QString cleanSource(const QStringList &roots) {
    QString git = QStandardPaths::findExecutable("git");
    if (git.isEmpty()) return {};
    for (const auto &path : sources(roots)) {
        auto query = [&](const QStringList &args) -> QString {
            QProcess process; process.start(git, QStringList{"-C", path} + args);
            if (!process.waitForFinished(1500) || process.exitCode() != 0) return {};
            return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
        };
        if (query({"remote", "get-url", "origin"}) != "https://github.com/VibeDarling/darling.git") continue;
        QString branch = query({"symbolic-ref", "--short", "HEAD"});
        if (branch != "main" && branch != "master") continue;
        QProcess status; status.start(git, {"-C", path, "status", "--porcelain"});
        if (status.waitForFinished(1500) && status.exitCode() == 0 && status.readAllStandardOutput().trimmed().isEmpty()) return path;
    }
    return {};
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
