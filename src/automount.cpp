// SPDX-License-Identifier: GPL-3.0-or-later
#include "automount.h"
#include "appneedscache.h"
#include "core.h"
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

namespace AutoMount {
using LauncherCore::fail;

const char *const kPolkitMessage = "Darling Launcher needs administrator rights to mount your macOS volumes read-only so that it can import the apps and frameworks you selected.";

Need needed(const QList<SourceMount> &mounts, bool wantCache) {
    Need need;
    need.appVolume = true;
    for (const SourceCandidate &candidate : LauncherSources::candidates(mounts)) if (candidate.readable && candidate.apps) need.appVolume = false; // the cryptex image alone has no apps
    need.cacheVolume = wantCache && AppNeedsCache::findCacheVolume({}, mounts).isEmpty();
    return need;
}

QList<MacPartition> apfsPartitions(const QList<MacPartition> &partitions) {
    QList<MacPartition> list;
    for (const MacPartition &p : partitions) if (p.filesystem == "apfs") list << p;
    return list;
}

QList<MacPartition> discoverPartitions(QString *error) {
    const QString tool = QStandardPaths::findExecutable("lsblk");
    if (tool.isEmpty()) { fail(error, "lsblk is unavailable, so the partitions cannot be listed."); return {}; }
    QProcess process;
    process.start(tool, {"--json", "--bytes", "--output", "PATH,TYPE,FSTYPE,LABEL,MOUNTPOINTS,SIZE"});
    if (!process.waitForFinished(5000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        process.kill(); fail(error, "Partition discovery with lsblk failed."); return {};
    }
    return apfsPartitions(LauncherMount::parsePartitions(process.readAllStandardOutput(), error));
}

QString partitionLine(const MacPartition &p) {
    QString size = p.sizeBytes < 0 ? QString() : QString(", %1 GB").arg(double(p.sizeBytes) / 1e9, 0, 'f', 1);
    return QString("%1 (APFS%2%3)").arg(p.device, p.label.isEmpty() ? QString() : ", " + p.label, size);
}

QString explanation(const Need &need, const MacPartition &partition) {
    QStringList parts;
    if (need.appVolume) parts << "the macOS Preboot and System volumes";
    if (need.cacheVolume) parts << (need.appVolume ? "the macOS cryptex image" : "the macOS Preboot volume and the macOS cryptex image") + QString(" (it holds the dyld shared cache)");
    const QString what = parts.join(" and ");
    return "Reading the macOS volumes on this computer needs administrator rights, because the macOS APFS partition can only be opened "
           "as a raw disk device by root.\n\n"
           "Partition: " + partitionLine(partition) + "\n\n"
           "The launcher will mount " + what + ", read-only, under your own folder (/run/darling-launcher/<your user id> and the launcher's "
           "mounts folder in your home). It does not write to the macOS partition, does not change the partition table and copies nothing "
           "yet: copying only follows after a separate preview and your confirmation.\n\n"
           "The mounts stay until you reboot or unmount them.\n\n"
           "Next, your operating system will show its own authentication dialog (polkit). The launcher never sees or stores your password.";
}

QString manualInstructions() {
    return "Nothing was mounted. To do it yourself: mount the macOS Preboot and System volumes read-only with apfs-fuse (see the README, "
           "\"Mount macOS source\"), or use the Mount macOS source dialog of the launcher, then run the import again.";
}

bool commandFor(const StepRequest &request, const Tools &tools, MountCommand *command, QString *error) {
    if (!QRegularExpression(R"(^/dev/[A-Za-z0-9_/-]+$)").match(request.device).hasMatch() || request.device.contains(".."))
        return fail(error, "Choose a detected device path under /dev");
    if (request.kind == Kind::CacheVolume)
        return LauncherMount::makeCryptexCommand(tools.python, tools.cryptexScript, tools.mountRoot, tools.apfsFuse, request.device, command, error);
    if (tools.python.isEmpty()) return fail(error, LauncherMount::noPythonMessage());
    if (!QDir::isAbsolutePath(tools.python) || !QFileInfo(tools.python).isExecutable() || !QDir::isAbsolutePath(tools.mountScript) || !QFileInfo(tools.mountScript).isFile())
        return fail(error, "The mount helper is unavailable");
    if (command) *command = {tools.python, {"-I", tools.mountScript, "--device=" + request.device}};
    return true;
}

Outcome ensureMounted(bool wantCache, const MacPartition &partition, Step &step, const Hooks &hooks) {
    Outcome outcome;
    const Need need = needed(hooks.mounts(), wantCache);
    if (!need.any()) { outcome.state = Outcome::State::AlreadyMounted; outcome.message = "The macOS volumes are already mounted and readable."; return outcome; }
    if (partition.device.isEmpty()) { outcome.message = "No APFS partition was chosen, so nothing can be mounted."; return outcome; }
    if (!hooks.consent(explanation(need, partition))) { outcome.state = Outcome::State::Cancelled; outcome.message = manualInstructions(); return outcome; }
    const auto satisfied = [&](Kind kind) {
        const Need now = needed(hooks.mounts(), wantCache);
        return kind == Kind::AppVolumes ? !now.appVolume : !now.cacheVolume;
    };
    for (const Kind kind : {Kind::AppVolumes, Kind::CacheVolume}) {
        if (kind == Kind::AppVolumes ? !need.appVolume : !need.cacheVolume) continue;
        outcome.stepsRun << (kind == Kind::AppVolumes ? "app volumes" : "cache volume");
        const StepResult result = step.run({kind, partition.device});
        if (result.crashed) { outcome.message = "The mount helper crashed."; return outcome; }
        if (result.exitCode != 0) {
            outcome.state = result.exitCode == 126 ? Outcome::State::Cancelled : Outcome::State::Failed;
            outcome.message = LauncherMount::exitDescription(result.exitCode);
            return outcome;
        }
        bool ok = satisfied(kind);
        for (int i = 0; !ok && i < hooks.checks; ++i) { if (hooks.sleep) hooks.sleep(1000); ok = satisfied(kind); }
        if (!ok) { outcome.message = "The mount helper finished but the volume is not readable yet. Check the helper output and try again."; return outcome; }
    }
    outcome.state = Outcome::State::Mounted; outcome.message = "The macOS volumes are mounted read-only.";
    return outcome;
}
}
