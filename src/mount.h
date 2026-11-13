// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QObject>
#include <QProcess>
#include <QList>
#include <QStringList>

struct MacPartition {
    QString device;
    QString filesystem;
    QString label;
    bool mounted = false;
    QStringList mountPoints;
};

struct MountCommand { QString program; QStringList arguments; };

namespace LauncherMount {
inline const QString pkexecPath = QStringLiteral("/usr/bin/pkexec");
inline const QString umountPath = QStringLiteral("/usr/bin/umount");
// Canonical path of an executable that only root (or extraOwner, when non-zero) can change, or empty with error set.
QString trustedExecutable(const QString &path, unsigned extraOwner, QString *error);
QList<MacPartition> parsePartitions(const QByteArray &json, QString *error);
// Root-owned directory under which tools/mount-macos.py mounts for this user (its MOUNT_PARENT/<uid>).
QString mountParent();
// True only for a mount point the helper created: a direct, canonical child of a root-owned mountParent().
// parent and extraOwner exist so tests can use a directory they own; production callers pass only the path.
bool isHelperMount(const QString &path, const QString &parent = mountParent(), unsigned extraOwner = 0);
// Canonical path of a regular file (executable when asked) whose whole resolved path only root, or extraOwner
// when non-zero, can change; empty with error set otherwise.
// The interpreter root runs through pkexec, chosen and validated at configure time (cmake/SelectPython.cmake).
QString launcherPython();  // empty when no trusted interpreter was found at configure time
QString noPythonMessage();
// launcherPython() (or any configured path) if it passes trustedFile; empty with error set otherwise, including
// the no-interpreter case, so the actions never reach pkexec.
QString trustedInterpreter(const QString &configured, unsigned extraOwner, QString *error);
QString trustedFile(const QString &path, unsigned extraOwner, bool executable, QString *error);
// The cryptex script and its sibling mount-macos.py must both pass trustedFile; root loads them both.
bool trustedCryptexScript(const QString &script, unsigned extraOwner, QString *error);
QString exitDescription(int code);
// The directory names are mirrored in tools/mount-cryptex.py.
QString cryptexMountDirectory(const QString &mountRoot);
// The root Unmount targets changes only when the script reports the pending root's image mounted and verified.
QString adoptCryptexRoot(const QString &current, const QString &pending, const QString &mountedPath);
// Mirrors resolve_home_prefix in tools/mount-cryptex.py: resolves a symlinked home prefix (/home -> /var/home).
// Returns an empty string when the home path passes through a dangling symlink (the script refuses it too).
QString resolveHomePrefix(const QString &mountRoot, const QString &home);
// The passwd entry's home directory, cleaned: the script uses the same source, never $HOME. Empty if there is none.
QString passwdHome();
bool makeCryptexCommand(const QString &python, const QString &script, const QString &mountRoot,
                        const QString &apfsFuse, const QString &device,
                        MountCommand *command, QString *error);
// Runs the script's privileged --unmount request (it asks for authorization itself).
bool makePrebootUnmountCommand(const QString &python, const QString &script, MountCommand *command, QString *error);
bool makeCryptexUnmountCommand(const QString &fusermount, const QString &mountRoot,
                               MountCommand *command, QString *error);
}

class MountRunner : public QObject {
    Q_OBJECT
public:
    explicit MountRunner(QObject *parent = nullptr);
    ~MountRunner() override;
    void start(const MountCommand &command);
signals:
    void output(const QString &text);
    void completed(bool success, const QString &message);
private:
    QProcess process;
};
