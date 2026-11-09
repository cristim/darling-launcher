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

enum class MountBackend { Kernel, ApfsFuse };
struct MountCommand { QString program; QStringList arguments; };

namespace LauncherMount {
QList<MacPartition> parsePartitions(const QByteArray &json, QString *error);
bool makeCommand(const MacPartition &partition, const QString &directory, MountBackend backend,
                 int volumeIndex, unsigned uid, unsigned gid, const QString &pkexec,
                 const QString &mountTool, MountCommand *command, QString *error);
// Canonical path of a regular file (executable when asked) whose whole resolved path only root, or extraOwner
// when non-zero, can change; empty with error set otherwise.
QString trustedFile(const QString &path, unsigned extraOwner, bool executable, QString *error);
// The cryptex script and its sibling mount-macos.py must both pass trustedFile; root loads them both.
bool trustedCryptexScript(const QString &script, unsigned extraOwner, QString *error);
QString exitDescription(int code);
// The directory names are mirrored in tools/mount-cryptex.py.
QString cryptexMountDirectory(const QString &mountRoot);
// Mirrors resolve_home_prefix in tools/mount-cryptex.py: resolves a symlinked home prefix (/home -> /var/home).
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
