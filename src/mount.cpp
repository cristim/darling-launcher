// SPDX-License-Identifier: GPL-3.0-or-later
#include "mount.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

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
}

namespace LauncherMount {
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

bool makeCommand(const MacPartition &partition, const QString &directory, MountBackend backend,
                 int volumeIndex, unsigned uid, unsigned gid, const QString &pkexec,
                 const QString &mountTool, MountCommand *command, QString *error) {
    if (!QRegularExpression(R"(^/dev/[A-Za-z0-9_/-]+$)").match(partition.device).hasMatch() || partition.device.contains(".."))
        return fail(error, "Choose a detected device path under /dev");
    if (partition.mounted) return fail(error, "This partition is already mounted; use mounted-volume discovery");
    if (partition.filesystem != "apfs" && partition.filesystem != "hfs" && partition.filesystem != "hfsplus")
        return fail(error, "Unsupported filesystem");
    QFileInfo target(directory);
    if (!QDir::isAbsolutePath(directory) || target.canonicalFilePath().isEmpty() || target.isSymLink() || !target.isDir() || target.canonicalFilePath() == "/")
        return fail(error, "Choose an existing mount directory");
    if (!QDir(directory).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).isEmpty())
        return fail(error, "Mount directory must be empty");
    for (const auto &path : {pkexec, mountTool})
        if (!QDir::isAbsolutePath(path) || !QFileInfo(path).isExecutable()) return fail(error, "Required mount or pkexec executable is unavailable");
    if (partition.filesystem == "apfs" && volumeIndex < 0) return fail(error, "Select an APFS container volume index explicitly");
    if (backend == MountBackend::ApfsFuse && partition.filesystem != "apfs") return fail(error, "APFS FUSE only supports APFS partitions");
    QString options = "ro,nodev,nosuid,noexec,uid=" + QString::number(uid) + ",gid=" + QString::number(gid);
    QStringList arguments{mountTool};
    if (backend == MountBackend::ApfsFuse) {
        arguments << "-v" << QString::number(volumeIndex) << "-o" << options + ",allow_other" << partition.device << target.canonicalFilePath();
    } else {
        if (partition.filesystem == "apfs") options += ",vol=" + QString::number(volumeIndex);
        arguments << "-i" << "-t" << partition.filesystem << "-o" << options << "--" << partition.device << target.canonicalFilePath();
    }
    if (command) *command = {pkexec, arguments};
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
