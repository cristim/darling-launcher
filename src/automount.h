// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "mount.h"
#include "sources.h"
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>

// The explained, consent-first way to get a macOS source mounted for an app import. It adds no privileged code: the
// privileged work stays in the existing root-owned helpers (tools/mount-macos.py, tools/mount-cryptex.py), which ask
// polkit themselves. This module decides whether anything has to be mounted, shows the user why before any
// authorization prompt, runs the helpers through a Step interface (a fake in tests) and checks the result.
// The launcher never asks for or sees a password.
namespace AutoMount {
// What is still missing. The app volume (System volume with the apps and frameworks) is always wanted; the cryptex OS
// image, which holds the dyld cache, only when the cache step is.
struct Need {
    bool appVolume = false, cacheVolume = false;
    bool any() const { return appVolume || cacheVolume; }
};
Need needed(const QList<SourceMount> &mounts, bool wantCache);

// APFS partitions a macOS install could be on, from a parsed lsblk listing; empty when none.
QList<MacPartition> apfsPartitions(const QList<MacPartition> &partitions);
// Runs lsblk (blocking, 5 s) and parses it; *error is set on failure.
QList<MacPartition> discoverPartitions(QString *error);

// The text shown before any authorization prompt (the GUI dialog and the CLI use exactly this).
QString explanation(const Need &need, const MacPartition &partition);
QString partitionLine(const MacPartition &partition);
// What to do by hand when the user declines.
QString manualInstructions();
// One sentence for the polkit dialog; the policy file template carries the same text.
extern const char *const kPolkitMessage;

enum class Kind { AppVolumes, CacheVolume };
struct StepRequest { Kind kind = Kind::AppVolumes; QString device; };
struct StepResult { int exitCode = 0; bool crashed = false; QString output; };
class Step {
public:
    virtual ~Step() = default;
    virtual StepResult run(const StepRequest &request) = 0;
};

// The argv the real step runs: the existing helpers, fixed arguments, a validated /dev path, nothing built by a shell.
struct Tools { QString python, mountScript, cryptexScript, apfsFuse, mountRoot; };
bool commandFor(const StepRequest &request, const Tools &tools, MountCommand *command, QString *error);

struct Outcome {
    enum class State { AlreadyMounted, Cancelled, Mounted, Failed } state = State::Failed;
    QString message;
    QStringList stepsRun; // for the log: which requests reached the helper
};
struct Hooks {
    std::function<QList<SourceMount>()> mounts;     // current mount table
    std::function<bool(const QString &)> consent;   // shown the explanation; true = Continue
    std::function<void(int ms)> sleep;              // between re-checks after a step
    int checks = 10;                                // re-checks of the mount table after a step (about 1 s apart)
};
// Nothing needed: AlreadyMounted and the step is never called. Otherwise the explanation goes to `consent` first; a
// refusal is Cancelled (with the manual instructions) and the step is never called. Then one step per missing part, and
// the result is believed only when the mount table shows it (a helper that exits 0 but mounted nothing is Failed).
Outcome ensureMounted(bool wantCache, const MacPartition &partition, Step &step, const Hooks &hooks);
}
