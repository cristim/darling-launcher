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
