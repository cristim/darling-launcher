// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QProcessEnvironment>
#include <QString>

// The environment for a guest run: no DYLD_* variable from the launcher's own environment reaches Darling, and a
// prefix that asked for the dyld cache gets exactly the two variables below and nothing else.
namespace LaunchEnv {
extern const char *const kSplitDirVar;  // DYLD_SHARED_CACHE_SPLIT_DIR: the store directory as the guest sees it
extern const char *const kPolicyVar;    // DYLD_SHARED_CACHE_SPLIT_POLICY: the policy file as the guest sees it

// Maintenance runs (prefix checks, repair, shutdown, shell, Brewfile): only strips DYLD_*. The cache variables apply to
// the user's app launch alone, because dyld picks the cache file by each process's architecture and halts when the
// directory has none for it, so every guest process of an arm64 tool would otherwise be at risk.
void stripDyld(QProcessEnvironment *env);

// The environment of one guest run. A maintenance run only strips DYLD_*. An app launch gets the cache variables when
// the prefix asked for the cache AND the app's executable will run as arm64e: dyld picks the cache file by the
// process's own subtype and halts when the directory has none for it, and the cache is arm64e. When the subtype cannot
// be determined, or the executable has an arm64 slice as well (which one runs is not known here), the variables are
// withheld and *note says why; the caller logs it. Child processes still inherit the variables, so the cache needs a
// dyld that continues without a cache when the file for a process's arch is missing.
enum class RunKind { Maintenance, App };
bool forRun(RunKind kind, QProcessEnvironment *env, const QString &prefix, const QString &storeRoot, const QString &executableRel,
            QString *error, QString *note);

// Removes every DYLD_* variable from *env, then, when the prefix's dyld-cache config wants the cache, sets the two
// variables. Fails loudly (and leaves *env untouched) when the config is invalid, the store is not complete, or the
// policy file does not match the config; the caller must not start the guest then. An empty prefix only strips.
bool guestEnvironment(QProcessEnvironment *env, const QString &prefix, const QString &storeRoot, QString *error);
}
