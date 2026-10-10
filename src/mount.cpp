// SPDX-License-Identifier: GPL-3.0-or-later
#include "mount.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <sys/stat.h>
#include <unistd.h>

namespace {
bool fail(QString *error, const QString &message) { if (error) *error = message; return false; }
void collect(const QJsonArray &devices, QList<MacPartition> *result) {
    for (const auto &value : devices) {
        const auto item = value.toObject();
        const QString fs = item.value("fstype").toString().toLower();
        if (fs == "apfs" || fs == "hfs" || fs == "hfsplus") {
            QStringList points;
            for (const auto &point : item.value("mountpoints").toArray())
                if (!point.toString().isEmpty()) points << point.toString();
            result->append({item.value("path").toString(), fs, item.value("label").toString(), !points.isEmpty(), points});
        }
        collect(item.value("children").toArray(), result);
    }
}
// lstat every component of an already canonical path; returns the first one that is a symlink,
// not owned by root (or extraOwner when non-zero) or writable by group/others, or empty.
QString untrustedComponent(const QString &resolved, unsigned extraOwner, struct stat *last) {
    QStringList components{"/"};
    for (const QString &part : resolved.split('/', Qt::SkipEmptyParts))
        components << (components.last() == "/" ? "/" : components.last() + "/") + part;
    for (const QString &component : components)
        if (::lstat(QFile::encodeName(component).constData(), last) != 0 || S_ISLNK(last->st_mode)
            || (last->st_uid != 0 && (extraOwner == 0 || last->st_uid != extraOwner)) || (last->st_mode & (S_IWGRP | S_IWOTH)))
            return component;
    return {};
}
}

namespace LauncherMount {
QString trustedExecutable(const QString &path, unsigned extraOwner, QString *error) {
    const QString resolved = QDir::isAbsolutePath(path) ? QFileInfo(path).canonicalFilePath() : QString();
    if (resolved.isEmpty()) { fail(error, "Required helper is missing or not an absolute path: " + path); return {}; }
    struct stat status {};
    if (const QString bad = untrustedComponent(resolved, extraOwner, &status); !bad.isEmpty()) {
        fail(error, "Refusing to run " + path + " as root: " + bad + " is not root-owned or is writable by others");
        return {};
    }
    if (!S_ISREG(status.st_mode) || !(status.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) { fail(error, "Not an executable file: " + path); return {}; }
    return resolved;
}

QList<MacPartition> parsePartitions(const QByteArray &json, QString *error) {
    QJsonParseError parseError;
    auto document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.object().value("blockdevices").isArray()) {
        fail(error, "Could not parse partition discovery output"); return {};
    }
    QList<MacPartition> partitions;
    collect(document.object().value("blockdevices").toArray(), &partitions);
    return partitions;
}

QString mountParent() { return "/run/darling-launcher/" + QString::number(::getuid()); }

bool isHelperMount(const QString &path) {
    const QFileInfo info(path);
    struct stat status {};
    // The mounted root reports the caller's uid (uid= option), so only the parent chain is checked.
    return QDir::isAbsolutePath(path) && info.canonicalFilePath() == path && info.isDir() && info.absolutePath() == mountParent()
        && untrustedComponent(mountParent(), 0, &status).isEmpty() && S_ISDIR(status.st_mode);
}

QString exitDescription(int code) {
    if (code == 126) return "Mount authorization was cancelled. No retry was attempted.";
    if (code == 127) return "Mount authorization was denied or no polkit authentication agent is available. No retry was attempted.";
    return code == 0 ? "Mount command completed" : "Mount command failed with exit status " + QString::number(code);
}
}

MountRunner::MountRunner(QObject *parent) : QObject(parent) {
    process.setProcessChannelMode(QProcess::MergedChannels);
    connect(&process, &QProcess::readyReadStandardOutput, this, [this] { emit output(QString::fromLocal8Bit(process.readAllStandardOutput())); });
    connect(&process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        emit output(QString::fromLocal8Bit(process.readAllStandardOutput()));
        emit completed(code == 0 && status == QProcess::NormalExit, status == QProcess::CrashExit ? "Mount command crashed" : LauncherMount::exitDescription(code));
    });
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) emit completed(false, process.errorString());
    });
}
void MountRunner::start(const MountCommand &command) { process.start(command.program, command.arguments); }
MountRunner::~MountRunner() { process.disconnect(this); }
