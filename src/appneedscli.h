// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "automount.h"
#include <QStringList>
#include <QTextStream>
#include <functional>

// `darling-launcher app-import --prefix DIR --app REL [--volume DIR] [--also NAME[,NAME...]]
//                              [--with-cache [--cache-volume DIR] [--accept-apple-binaries]] (--dry-run | --yes)`
// The headless form of importing an app and the framework resources it needs. args are the arguments after "app-import".
//   --app REL       the bundle below the volume, e.g. System/Applications/Notes.app
//   --volume DIR    the mounted macOS System volume; without it the mounted source is found, and when there is none the
//                   explained mount flow runs (interactive terminals only: the explanation is printed and a "y" is read)
//   --dry-run       prints the plan (selected, optional and skipped frameworks, sizes, free space, the cache offer); copies nothing
//   --yes           imports the app when it is not in the prefix yet and the SELECTED framework resources; never the cache
//   --also NAMES    also imports these optional frameworks (they must be linked by the app and have resources)
//   --with-cache    also runs the separate dyld cache step (Apple executable binaries); it needs --accept-apple-binaries
//                   (printed notice) on top of --yes, and is refused otherwise before anything is copied
// Without --dry-run and --yes nothing is copied and the exit code is 2. Exit: 0 done or dry run, 1 refused or failed, 2 usage
// or missing confirmation.
struct AppImportEnv {
    std::function<bool()> interactive;                  // default: stdin is a terminal
    std::function<QString()> readLine;                  // default: one line of stdin
    AutoMount::Step *mountStep = nullptr;               // null: the mount flow is refused with the manual instructions
    std::function<QList<SourceMount>()> mounts;         // default: the live mount table
    std::function<QList<MacPartition>(QString *)> partitions; // default: lsblk
    std::function<void(int)> sleep;
    QString storeRoot;                                  // default: the launcher setting
    qint64 freeBytesOverride = -1;                      // test seam
};
int appImportMain(const QStringList &args, QTextStream &out, QTextStream &err, const AppImportEnv &env = {});
