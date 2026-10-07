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
            bool mounted = false;
            for (const auto &point : item.value("mountpoints").toArray())
                if (!point.toString().isEmpty()) mounted = true;
            result->append({item.value("path").toString(), fs, item.value("label").toString(), mounted});
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

#include <QStorageInfo>
#include <QFile>
#include <sys/stat.h>
#include <unistd.h>

QList<ApfsVolume> LauncherMount::parseVolumes(const QString &output) {
    QList<ApfsVolume> result;
    int active = -1;
    QRegularExpression heading(R"(^Volume (\d+) [0-9A-Fa-f-]{36}\s*$)");
    for (const auto &line : output.split('\n')) {
        auto match = heading.match(line);
        if (line.startsWith("Volume ")) active = -1;
        if (match.hasMatch()) {
            bool valid = false; int index = match.captured(1).toInt(&valid);
            if (valid && index >= 0 && index < 100) {
                bool duplicate = false; for (const auto &volume : result) if (volume.index == index) duplicate = true;
                if (!duplicate) { result.append({index, {}, true}); active = result.size() - 1; }
            }
        } else if (active >= 0) {
            if (line.startsWith("Name:")) result[active].name = line.mid(5).trimmed();
            if (line.startsWith("FileVault:")) result[active].encrypted = line.mid(10).trimmed() != "No";
        }
    }
    return result;
}
MountBatch::MountBatch(QObject *parent) : QObject(parent) {
    process.setProcessChannelMode(QProcess::MergedChannels);
    connect(&process, &QProcess::readyReadStandardOutput, this, [this] {
        const QString text = QString::fromUtf8(process.readAllStandardOutput()); transcript += text; emit output(text);
    });
    connect(&process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        transcript += QString::fromUtf8(process.readAllStandardOutput());
        if (code == 126 || code == 127) { emit output(LauncherMount::exitDescription(code)); emit completed(owned, true); return; }
        if (enumerating) {
            if (code == 0 && status == QProcess::NormalExit) volumes = LauncherMount::parseVolumes(transcript);
            else emit output("Volume enumeration failed for " + partitions.at(partitionIndex).device);
            nextVolume();
        } else {
            if (code == 0 && status == QProcess::NormalExit) {
                QStorageInfo storage(currentTarget); storage.refresh();
                if (storage.rootPath() == currentTarget && storage.isReadOnly() &&
                    QFileInfo(QString::fromLocal8Bit(storage.device())).canonicalFilePath() == QFileInfo(partitions.at(partitionIndex).device).canonicalFilePath()) {
                    owned << currentTarget; emit output("Verified read-only mount: " + currentTarget);
                } else emit output("Mount not verified: " + currentTarget);
            } else emit output("Mount failed: " + currentTarget);
            nextVolume();
        }
    });
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) { emit output(process.errorString()); emit completed(owned, false); }
    });
}
MountBatch::~MountBatch() { process.disconnect(this); }
void MountBatch::start(const QList<MacPartition> &inputs, const QString &directory, const QString &authorization,
                       const QString &kernel, const QString &fuse, const QString &utility) {
    if (process.state() != QProcess::NotRunning || partitionIndex != -1) return;
    partitions = inputs; root = directory; pkexec = authorization; mountTool = kernel; fuseTool = fuse; volumeTool = utility;
    if (!QDir::isAbsolutePath(root) || QFileInfo(root).isSymLink() || !QDir().mkpath(root) || QFileInfo(root).canonicalFilePath() != QDir::cleanPath(root)) {
        emit output("Choose an absolute batch mount root without symlink ancestors."); emit completed({}, false); return;
    }
    if (!QDir::isAbsolutePath(pkexec) || !QFileInfo(pkexec).isExecutable()) { emit output("pkexec is unavailable."); emit completed({}, false); return; }
    nextPartition();
}
void MountBatch::nextPartition() {
    ++partitionIndex;
    if (partitionIndex >= partitions.size()) { emit completed(owned, false); return; }
    const auto &partition = partitions.at(partitionIndex);
    struct stat info {};
    if (partition.mounted || ::stat(QFile::encodeName(partition.device).constData(), &info) != 0 || !S_ISBLK(info.st_mode)) {
        emit output("Skipping mounted or unavailable partition: " + partition.device); nextPartition(); return;
    }
    for (const auto &storage : QStorageInfo::mountedVolumes())
        if (QFileInfo(QString::fromLocal8Bit(storage.device())).canonicalFilePath() == QFileInfo(partition.device).canonicalFilePath()) {
            emit output("Already mounted: " + partition.device); nextPartition(); return;
        }
    if (partition.filesystem == "apfs") {
        if (!QDir::isAbsolutePath(fuseTool) || !QDir::isAbsolutePath(volumeTool) || !QFileInfo(fuseTool).isExecutable() || !QFileInfo(volumeTool).isExecutable()) {
            emit output("APFS FUSE and apfsutil are required to enumerate and mount every APFS volume."); nextPartition(); return;
        }
        enumerating = true; transcript.clear(); volumes.clear();
        emit output("Enumerating filesystem metadata: " + partition.device);
        process.start(pkexec, {volumeTool, partition.device});
    } else if (partition.filesystem == "hfs" || partition.filesystem == "hfsplus") {
        volumes = {{-1, partition.label, false}}; nextVolume();
    } else nextPartition();
}
void MountBatch::nextVolume() {
    if (volumes.isEmpty()) { nextPartition(); return; }
    const auto volume = volumes.takeFirst(); const auto partition = partitions.at(partitionIndex);
    if (volume.encrypted) { emit output("Skipping encrypted volume " + volume.name + "; unlock it separately."); nextVolume(); return; }
    currentTarget = root + '/' + QFileInfo(partition.device).fileName() + "-volume-" + QString::number(volume.index);
    if (QFileInfo(currentTarget).isSymLink() || !QDir().mkpath(currentTarget)) { emit output("Cannot create mount directory: " + currentTarget); nextVolume(); return; }
    QStorageInfo existing(currentTarget);
    if (existing.rootPath() == currentTarget) { emit output("Already a mount point: " + currentTarget); nextVolume(); return; }
    MountCommand command; QString error;
    if (!LauncherMount::makeCommand(partition, currentTarget, partition.filesystem == "apfs" ? MountBackend::ApfsFuse : MountBackend::Kernel,
                                   volume.index, getuid(), getgid(), pkexec, partition.filesystem == "apfs" ? fuseTool : mountTool, &command, &error)) {
        emit output(error); nextVolume(); return;
    }
    enumerating = false; transcript.clear(); emit output("Mounting read-only: " + volume.name + " → " + currentTarget);
    process.start(command.program, command.arguments);
}
