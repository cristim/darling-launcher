// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "libraryimport.h"

// Enable, disable and the per-prefix master switch for imported libraries. All of them take the launcher lock,
// refuse a running prefix and treat a file as removable only when it is ours: its sha256 equals the recorded one
// or the pending store file's (an interrupted import). The store copy is guest-writable and never a baseline.
// ImportOptions.abortAfter is called with "removed" (disable, switch off) or "upper" (enable, switch on).
bool disableLibrary(const QString &prefix, const QString &installName, QString *error, const ImportOptions &options = {});
// Restores the upper file from <store>/image or <store>/image.pending, whichever matches the recorded sha256.
// Refuses pinned and runtime-provided paths unless force, and a prefix whose master switch is off.
bool enableLibrary(const QString &prefix, const QString &runtimeRoot, const QString &installName, bool force,
                   QString *error, const ImportOptions &options = {});
// Off removes the upper files of all enabled libraries (their per-library flags are kept); files that are not ours
// are left alone and listed in *skipped (may be null). On restores the files and refuses if any library fails its
// checks. Failing mid-way is completed by rerunning. abortAfter also sees "item:<installName>" after each library.
bool setMasterSwitch(const QString &prefix, const QString &runtimeRoot, bool enabled, bool force, QStringList *skipped,
                     QString *error, const ImportOptions &options = {});
