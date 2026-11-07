// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "libraryimport.h"

enum class LibraryStatus {
    Builtin,     // no record or upper file here, but the Darling runtime provides it
    Imported,    // our upper file is in place, shadowing any runtime copy
    Missing,     // nothing provides it
    Disabled,    // recorded, upper file removed (per library or by the master switch)
    Interrupted, // an import, enable or disable was cut short; rerun it
    Foreign      // a file we did not write sits at the path
};
QString libraryStatusName(LibraryStatus status);

// runtimeRoot is darlingserver's libexec path, as for importImage. Fails closed when libraries.json or the
// files cannot be read.
bool libraryStatus(const QString &prefix, const QString &runtimeRoot, const QString &installName, LibraryStatus *out,
                   QString *error, qint64 maxImageBytes = qint64(512) << 20);
