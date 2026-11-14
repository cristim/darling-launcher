// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>

// Imports the on-disk resource parts of the macOS framework bundles whose code lives in the dyld shared cache:
// for every <volume>/System/Library/{Frameworks,PrivateFrameworks}/*.framework (and framework bundles nested in
// Versions/<v>/Frameworks) it copies Info.plist, version.plist and Resources/ to the same path in the prefix, keeping
// the Versions/Current layout and symlinks. Executable code is never copied: the framework binary, any file that starts
// with a Mach-O or fat magic (whatever its name), "#!" scripts, *.dylib/*.so/*.a/*.o, and the directories XPCServices,
// PlugIns, Helpers, MacOS and anything named *.app, *.xpc, *.appex, *.kext or *.framework inside Resources.
// Nothing in the prefix is ever replaced: an existing file that differs is reported as a conflict and left alone.
namespace FrameworkResources {
enum class Kind { Directory, File, Symlink };
struct Entry {
    Kind kind;
    QString rel;    // path below the volume and below the prefix (the same in both)
    qint64 size = 0; // File only
    QString target;  // Symlink only: the raw link text, copied verbatim
};
struct Plan {
    QList<Entry> entries; // only what is missing in the prefix, parents before children
    int frameworks = 0;   // framework bundles visited, nested ones included
    int files = 0, directories = 0, symlinks = 0; // entries to create
    quint64 bytes = 0;    // sum of the sizes of the files to create: the size estimate
    int alreadyPresent = 0; // files and symlinks identical to what the prefix has
    int skippedMachO = 0, skippedScripts = 0, skippedByName = 0;
    QStringList conflicts; // present in the prefix but different or of another kind: left unchanged
    QStringList refused;   // source items not imported: absolute, escaping or dangling symlinks, special files
    qint64 availableBytes = -1; // free space on the prefix filesystem

    // Free space left untouched after the copy, so a nearly full disk is not driven to zero.
    static constexpr qint64 kReserveBytes = 512ll * 1024 * 1024;
    bool enoughSpace() const { return availableBytes < 0 || qint64(bytes) + kReserveBytes <= availableBytes; }
};
using Cancelled = std::function<bool()>;

// Reads the volume and the prefix and writes nothing: this is the dry run and the size estimate.
bool buildPlan(const QString &volume, const QString &prefix, Plan *plan, QString *error, const Cancelled &cancelled = {});

QString formatBytes(quint64 bytes);
// Human-readable plan: sizes, counts, free space, the skipped Mach-O count and the first conflicts and refusals.
QString summarize(const Plan &plan);
}
