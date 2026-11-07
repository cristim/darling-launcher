// SPDX-License-Identifier: GPL-3.0-or-later
#include "librarystatus.h"
#include "core.h"
#include <QFileInfo>

QString libraryStatusName(LibraryStatus status) {
    switch (status) {
    case LibraryStatus::Builtin: return "builtin";
    case LibraryStatus::Imported: return "imported";
    case LibraryStatus::Missing: return "missing";
    case LibraryStatus::Disabled: return "disabled";
    case LibraryStatus::Interrupted: return "interrupted";
    case LibraryStatus::Foreign: return "foreign";
    }
    return {};
}

bool libraryStatus(const QString &prefix, const QString &runtimeRoot, const QString &installName, LibraryStatus *out,
                   QString *error, qint64 maxImageBytes) {
    if (!Libraries::validInstallName(installName, error)) return false;
    const QFileInfo root(runtimeRoot);
    if (!root.isAbsolute() || !root.isDir()) return LauncherCore::fail(error, "The runtime root must be an existing absolute directory: " + runtimeRoot);
    LibrariesFile file;
    if (!Libraries::load(prefix, &file, error)) return false;
    const LibraryRecord *record = nullptr;
    for (const LibraryRecord &r : file.libraries) if (r.installName == installName) record = &r;
    UpperState upper;
    if (!upperFileState(prefix, installName, record, maxImageBytes, &upper, error)) return false;
    const bool wanted = record && record->enabled && file.enabled;
    // The upper file is checked before the runtime: it shadows the runtime's copy.
    if (upper.exists) *out = !upper.ours ? LibraryStatus::Foreign : (upper.interrupted || !wanted) ? LibraryStatus::Interrupted : LibraryStatus::Imported;
    else if (record) *out = wanted ? LibraryStatus::Interrupted : LibraryStatus::Disabled;
    else *out = runtimeProvides(runtimeRoot, installName) ? LibraryStatus::Builtin : LibraryStatus::Missing;
    return true;
}
