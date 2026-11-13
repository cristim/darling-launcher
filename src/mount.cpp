// SPDX-License-Identifier: GPL-3.0-or-later
#include "mount.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
bool fail(QString *error, const QString &message) { if (error) *error = message; return false; }
// lstat every component of a canonical path; returns the first that is a symlink, not owned by root (or
// extraOwner when non-zero) or writable by group/others, or an empty string.
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
bool hasDanglingSymlink(const QString &path) {
    QString current;
    for (const QString &part : path.split('/', Qt::SkipEmptyParts)) {
        current += "/" + part;
        const QFileInfo info(current);
        if (info.isSymLink() && !info.exists()) return true;
    }
    return false;
}
bool validRoot(const QString &root) {
    return QDir::isAbsolutePath(root) && QDir::cleanPath(root) == root && root != "/";
}
void collect(const QJsonArray &devices, QList<MacPartition> *result) {
    for (const auto &value : devices) {
        const auto item = value.toObject();
        // Root parses these filesystems, and anyone can attach a loop device holding an image of their choosing.
        if (item.value("type").toString() == "loop") continue;
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

bool isHelperMount(const QString &path, const QString &parent, unsigned extraOwner) {
    const QFileInfo info(path);
    struct stat status {};
    // The mounted root reports the caller's uid (uid= option), so only the parent chain is checked.
    return QDir::isAbsolutePath(path) && info.canonicalFilePath() == path && info.isDir() && info.absolutePath() == parent
        && untrustedComponent(parent, extraOwner, &status).isEmpty() && S_ISDIR(status.st_mode);
}

QString resolveHomePrefix(const QString &mountRoot, const QString &home) {
    const QString root = QDir::cleanPath(mountRoot), cleanHome = QDir::cleanPath(home);
    if (cleanHome.isEmpty() || (root != cleanHome && !root.startsWith(cleanHome + "/"))) return root;
    if (hasDanglingSymlink(cleanHome)) return {};  // the script refuses this too
    // Like Python's realpath: resolve the longest existing prefix and keep the missing tail as written.
    QString existing = cleanHome, tail;
    while (existing != "/" && !QFileInfo::exists(existing)) {
        const int slash = existing.lastIndexOf('/');
        if (slash < 0) return root;
        tail = existing.mid(slash) + tail; existing = slash == 0 ? QStringLiteral("/") : existing.left(slash);
    }
    QString resolved = QFileInfo(existing).canonicalFilePath();
    if (resolved.isEmpty()) return root;
    if (resolved == "/") resolved.clear();
    return resolved + tail + root.mid(cleanHome.size());
}

QString adoptCryptexRoot(const QString &current, const QString &pending, const QString &mountedPath) {
    return !pending.isEmpty() && mountedPath == cryptexMountDirectory(pending) ? pending : current;
}

QString passwdHome() {
    struct passwd entry {}; struct passwd *found = nullptr; QByteArray buffer(4096, 0);
    if (::getpwuid_r(::getuid(), &entry, buffer.data(), buffer.size(), &found) != 0 || !found) return {};
    return QDir::cleanPath(QString::fromLocal8Bit(entry.pw_dir));
}

QString launcherPython() { return QStringLiteral(LAUNCHER_PYTHON); }

QString noPythonMessage() {
    return "No trusted Python 3.8+ was found when the launcher was configured, so the cryptex and Preboot actions are disabled; see the README (Mount the cryptex OS image).";
}

QString trustedInterpreter(const QString &configured, unsigned extraOwner, QString *error) {
    if (configured.isEmpty()) { fail(error, noPythonMessage()); return {}; }
    return trustedFile(configured, extraOwner, true, error);
}

QString trustedFile(const QString &path, unsigned extraOwner, bool executable, QString *error) {
    const QString resolved = QDir::isAbsolutePath(path) ? QFileInfo(path).canonicalFilePath() : QString();
    if (resolved.isEmpty()) { fail(error, "Required file is missing or not an absolute path: " + path); return {}; }
    struct stat status {};
    if (const QString bad = untrustedComponent(resolved, extraOwner, &status); !bad.isEmpty()) {
        fail(error, "Refusing to run " + path + " as root: " + bad + " is not root-owned or is writable by others");
        return {};
    }
    if (!S_ISREG(status.st_mode) || (executable && !(status.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))) { fail(error, "Not a usable file: " + path); return {}; }
    return resolved;
}

bool trustedCryptexScript(const QString &script, unsigned extraOwner, QString *error) {
    const QString resolved = trustedFile(script, extraOwner, true, error);
    return !resolved.isEmpty() && !trustedFile(QFileInfo(resolved).absolutePath() + "/mount-macos.py", extraOwner, false, error).isEmpty();
}

QString cryptexMountDirectory(const QString &mountRoot) { return mountRoot + "/macos-cryptex-os"; }

bool makeCryptexCommand(const QString &python, const QString &script, const QString &mountRoot,
                        const QString &apfsFuse, const QString &device,
                        MountCommand *command, QString *error) {
    if (python.isEmpty()) return fail(error, noPythonMessage());
    if (!validRoot(mountRoot)) return fail(error, "Choose an absolute mount root");
    if (!device.isEmpty() && (!QRegularExpression(R"(^/dev/[A-Za-z0-9_/-]+$)").match(device).hasMatch() || device.contains("..")))
        return fail(error, "Choose a detected device path under /dev");
    for (const auto &tool : {python, apfsFuse})
        if (!QDir::isAbsolutePath(tool) || !QFileInfo(tool).isExecutable()) return fail(error, "Python or apfs-fuse is unavailable");
    if (!QDir::isAbsolutePath(script) || !QFileInfo(script).isFile()) return fail(error, "The cryptex mount script is unavailable");
    QStringList arguments{"-I", script, "--mount-root", mountRoot, "--apfs-fuse", apfsFuse};
    if (!device.isEmpty()) arguments << "--device" << device;
    if (command) *command = {python, arguments};
    return true;
}

bool makePrebootUnmountCommand(const QString &python, const QString &script, MountCommand *command, QString *error) {
    if (python.isEmpty()) return fail(error, noPythonMessage());
    if (!QDir::isAbsolutePath(python) || !QFileInfo(python).isExecutable()) return fail(error, "Python is unavailable");
    if (!QDir::isAbsolutePath(script) || !QFileInfo(script).isFile()) return fail(error, "The cryptex mount script is unavailable");
    if (command) *command = {python, {"-I", script, "--unmount"}};
    return true;
}

bool makeCryptexUnmountCommand(const QString &fusermount, const QString &mountRoot, MountCommand *command, QString *error) {
    if (!validRoot(mountRoot)) return fail(error, "Choose an absolute mount root");
    if (!QDir::isAbsolutePath(fusermount) || !QFileInfo(fusermount).isExecutable()) return fail(error, "fusermount is unavailable");
    if (command) *command = {fusermount, {"-u", "--", cryptexMountDirectory(mountRoot)}};
    return true;
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
