// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "frameworkresources.h"
#include "macho.h"
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>

// What an imported macOS app needs from the volume it came from, worked out from NAMES only and copied only after a
// confirmation. The data sources, in order: the app's CFBundleExecutable, the dylib install names in that executable's
// load commands (header and load commands only, like `otool -L`), the same for libraries embedded in the app bundle,
// and the presence (stat) of framework bundle directories and binaries on the volume. System frameworks and the dyld
// cache are never opened by this code: the dependency names of Apple's own frameworks are not read.
//
// A linked framework is imported only when (a) the app links it, (b) it has resource files the framework resource walker
// would copy, (c) Darling does not provide it and the prefix does not already have those files, and (d) it is either
// part of the app's own family (selected by default) or the user chose it (--also, or a checkbox).
namespace AppNeeds {

struct Linked {
    QString installName; // absolute, as written in the load command
    QString via;         // empty: the app executable; otherwise the embedded library (path below the volume) that names it
};
struct Names {
    QString appName;      // bundle base name, e.g. "Notes"
    QString executable;   // CFBundleExecutable
    QString executableRel; // Contents/MacOS/<executable> below the volume
    QList<Linked> linked; // absolute names, unique, discovery order
    QStringList unresolvedRelative; // @rpath/@executable_path/... names with no file inside the app bundle
    QStringList embeddedRead;       // embedded images whose names were read (paths below the volume)
};
// Reads the dependency names of the image at `rel` below `volume`. The default reader opens the file without following
// symlinks and reads only the fat header and the chosen slice's header and load commands.
using NameReader = std::function<bool(const QString &volume, const QString &rel, QList<MachO::Dependency> *names, QString *error)>;
NameReader windowedReader();

// `appBundleRel` is the bundle below the volume (System/Applications/Notes.app). Embedded images are followed through
// names that start with @rpath/ or @executable_path/../Frameworks/ and resolve to a regular file in Contents/Frameworks
// of the same bundle; every other absolute name is recorded and never read.
bool linkedNames(const QString &volume, const QString &appBundleRel, Names *out, QString *error, const NameReader &read = windowedReader());

enum class Status { Selected, Optional, Skipped };
struct Item {
    QString name;        // framework base name
    QString installName;
    QString via;
    Status status = Status::Skipped;
    QString reason;      // why selected, optional or skipped
    int files = 0, directories = 0, symlinks = 0;
    quint64 bytes = 0, apparentBytes = 0; // on disk, and sum of the lengths
};
struct Plan {
    QString appName, appBundleRel, executableRel;
    QList<Item> items;               // every linked framework bundle name, in link order
    QStringList linkedInstallNames;  // every absolute name the app links, discovery order
    QStringList notFrameworkBundles; // linked names that are not framework bundles (system dylibs)
    QStringList unresolvedRelative;
    int linkedCount = 0;
    qint64 availableBytes = -1;
    int selectedCount() const;
    int selectedFiles() const;
    quint64 selectedBytes() const;
    QStringList selectedNames() const;
    QList<const Item *> byStatus(Status status) const;
};
struct Options {
    QStringList also;            // optional frameworks the user chose (base names)
    NameReader reader;           // default: windowedReader()
    std::function<qint64(const QString &)> freeBytes; // test seam: free space of the prefix
};
// Reads the volume and the prefix, writes nothing. `also` names that are not linked, are skipped or are unknown fail.
bool buildPlan(const QString &volume, const QString &prefix, const QString &appBundleRel, const Options &options, Plan *plan, QString *error);

// Human-readable preview: selected, optional and skipped frameworks with reasons, totals and free space.
QString summarize(const Plan &plan);

struct Applied {
    FrameworkResources::Result result;
    QStringList bundles;
};
// Copies the selected frameworks' resources. Refuses (and copies nothing) unless `confirmed` is true, when nothing is
// selected, or when the free space is short; re-plans right before copying; appends one row to the prefix's
// app-imports.md log. Never replaces anything and never copies code (the walker's rules).
bool applyResources(const QString &volume, const QString &prefix, const Plan &plan, bool confirmed, const Options &options,
                    Applied *applied, QString *error, const FrameworkResources::Progress &progress = {}, const FrameworkResources::Cancelled &cancelled = {});

// <prefix>/.darling-launcher/app-imports.md: one table row per import step, appended.
QString logPath(const QString &prefix);
bool appendLog(const QString &prefix, const QString &app, const QString &step, const QString &items, const QString &counts, const QString &note, QString *error);
}
