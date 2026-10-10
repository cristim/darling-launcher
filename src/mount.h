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
bool isHelperMount(const QString &path);
QString exitDescription(int code);
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
