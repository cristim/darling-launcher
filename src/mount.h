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
};

enum class MountBackend { Kernel, ApfsFuse };
struct MountCommand { QString program; QStringList arguments; };

namespace LauncherMount {
QList<MacPartition> parsePartitions(const QByteArray &json, QString *error);
bool makeCommand(const MacPartition &partition, const QString &directory, MountBackend backend,
                 int volumeIndex, unsigned uid, unsigned gid, const QString &pkexec,
                 const QString &mountTool, MountCommand *command, QString *error);
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

struct ApfsVolume { int index; QString name; bool encrypted = false; };
namespace LauncherMount {
QList<ApfsVolume> parseVolumes(const QString &output);
}
class MountBatch : public QObject {
    Q_OBJECT
public:
    explicit MountBatch(QObject *parent = nullptr);
    ~MountBatch() override;
    void start(const QList<MacPartition> &partitions, const QString &root, const QString &pkexec,
               const QString &mountTool, const QString &fuseTool, const QString &volumeTool);
signals:
    void output(const QString &text);
    void completed(const QStringList &mounts, bool cancelled);
private:
    QProcess process;
    QList<MacPartition> partitions;
    QList<ApfsVolume> volumes;
    QString root, pkexec, mountTool, fuseTool, volumeTool, currentTarget, transcript;
    QStringList owned;
    int partitionIndex = -1;
    bool enumerating = false;
    void nextPartition();
    void nextVolume();
};
