// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "prefixstate.h"
#include <QByteArray>
#include <QString>
#include <QStringList>

// Per-prefix choice of which libraries dyld serves from the private cache store instead of Darling's own copies.
// <prefix>/.darling-launcher/dyld-cache.json (the choice) and dyld-cache.policy (what dyld reads, derived from it).
// Darling's own copies win by default: a library is served from the cache only when it is listed here.
// Future mode (documented, not implemented): "cache-first" with a deny-list, serving everything the cache has
// except the listed libraries.
enum class DyldCacheMode { Off, AllowList };

struct DyldCacheConfig {
    DyldCacheMode mode = DyldCacheMode::Off;  // Off is the master switch: the selection below is kept while off
    QString store;                            // store id, empty until one is chosen
    QStringList served;                       // install names dyld serves from the cache
};

namespace DyldCacheConfigFile {
constexpr int kMaxServed = 64;
// The only libraries that may be selected. A library belongs here once its cache copy has been shown to run under
// Darling's dyld while libSystem and the rest of the runtime stay Darling's own; today that is libz alone (a leaf
// library, and the single entry of dyld's built-in allow-list).
const QStringList &validatedLibraries();
// Where dyld finds the policy file, as a path inside the guest (the prefix is the guest's root).
extern const char *const kGuestPolicyPath;

QString configPath(const QString &prefix);
QString policyPath(const QString &prefix);
bool validate(const DyldCacheConfig &config, QString *error);
// allow-list mode and at least one library: the settings under which the prefix asks dyld for the cache.
bool wantsCache(const DyldCacheConfig &config);
// A missing file is the default (off). Anything unreadable or invalid fails closed.
bool load(const QString &prefix, DyldCacheConfig *out, QString *error);
bool serialize(const DyldCacheConfig &config, QByteArray *json, QString *error);
// Policy file v1 for dyld: a header line, then one canonical install name per line.
QByteArray renderPolicy(const QStringList &served);
// Writes the policy file first (removing it when the prefix does not want the cache), then the config. Takes the
// launcher lock and refuses a prefix that may be running.
bool save(const QString &prefix, const DyldCacheConfig &config, QString *error, const PrefixState::ProcFs &procFs = {});
}
