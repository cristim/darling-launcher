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

// Removes every DYLD_* variable from *env, then, when the prefix's dyld-cache config wants the cache, sets the two
// variables. Fails loudly (and leaves *env untouched) when the config is invalid, the store is not complete, or the
// policy file does not match the config; the caller must not start the guest then. An empty prefix only strips.
bool guestEnvironment(QProcessEnvironment *env, const QString &prefix, const QString &storeRoot, QString *error);
}
