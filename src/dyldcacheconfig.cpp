// SPDX-License-Identifier: GPL-3.0-or-later
#include "dyldcacheconfig.h"
#include "core.h"
#include "dyldcachestore.h"
#include "libraries.h"
#include "safefs.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <unistd.h>

namespace {
using LauncherCore::fail;
const qint64 kMaxFileBytes = 1 << 20;
const QString kConfigRel = QStringLiteral(".darling-launcher/dyld-cache.json");
const QString kPolicyRel = QStringLiteral(".darling-launcher/dyld-cache.policy");
const char *modeName(DyldCacheMode m) { return m == DyldCacheMode::AllowList ? "allowlist" : "off"; }
}

namespace DyldCacheConfigFile {
const char *const kGuestPolicyPath = "/.darling-launcher/dyld-cache.policy";

const QStringList &validatedLibraries() {
    static const QStringList list = {"/usr/lib/libz.1.dylib"};
    return list;
}
QString configPath(const QString &prefix) { return prefix + "/" + kConfigRel; }
QString policyPath(const QString &prefix) { return prefix + "/" + kPolicyRel; }

bool validate(const DyldCacheConfig &c, QString *error) {
    if (!c.store.isEmpty() && !DyldCacheStore::validStoreId(c.store)) return fail(error, "Invalid cache store id");
    if (c.served.size() > kMaxServed) return fail(error, "Too many cached libraries");
    QSet<QString> seen;
    for (const QString &name : c.served) {
        if (!Libraries::validInstallName(name, error)) return false;
        if (!validatedLibraries().contains(name)) return fail(error, name + " is not validated to run from the dyld cache");
        if (seen.contains(name)) return fail(error, "Duplicate cached library: " + name);
        seen.insert(name);
    }
    if (c.mode == DyldCacheMode::AllowList && c.store.isEmpty()) return fail(error, "The cache is turned on but no store is chosen");
    return true;
}

bool wantsCache(const DyldCacheConfig &c) { return c.mode == DyldCacheMode::AllowList && !c.served.isEmpty(); }

bool serialize(const DyldCacheConfig &c, QByteArray *json, QString *error) {
    if (!validate(c, error)) return false;
    *json = QJsonDocument(QJsonObject{{"version", 1}, {"mode", modeName(c.mode)}, {"store", c.store},
                                      {"served", QJsonArray::fromStringList(c.served)}}).toJson(QJsonDocument::Indented);
    return true;
}

bool load(const QString &prefix, DyldCacheConfig *out, QString *error) {
    *out = {};
    QByteArray raw; bool exists = false;
    if (!SafeFs::readFile(prefix, kConfigRel, kMaxFileBytes, &raw, &exists, error)) return false;
    if (!exists) return true;
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(raw, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) return fail(error, "dyld-cache.json is not valid JSON");
    const auto o = doc.object();
    if (!o.value("version").isDouble() || o.value("version").toInt(-1) != 1) return fail(error, "Unsupported dyld-cache.json version");
    if (!o.value("mode").isString() || !o.value("store").isString() || !o.value("served").isArray())
        return fail(error, "dyld-cache.json has a field of the wrong type");
    DyldCacheConfig c;
    c.store = o.value("store").toString();
    const QString mode = o.value("mode").toString();
    if (mode == "off") c.mode = DyldCacheMode::Off; else if (mode == "allowlist") c.mode = DyldCacheMode::AllowList;
    else return fail(error, "Unknown dyld-cache.json mode");
    const auto served = o.value("served").toArray();
    if (served.size() > kMaxServed) return fail(error, "dyld-cache.json lists too many libraries");
    for (const auto &v : served) {
        if (!v.isString()) return fail(error, "dyld-cache.json 'served' must hold strings");
        c.served << v.toString();
    }
    if (!validate(c, error)) return false;
    *out = c;
    return true;
}

QByteArray renderPolicy(const QStringList &served) {
    QByteArray out = "darling-dyld-cache-policy 1\n";
    for (const QString &name : served) out += name.toUtf8() + "\n";
    return out;
}

bool save(const QString &prefix, const DyldCacheConfig &config, QString *error, const PrefixState::ProcFs &procFs) {
    QByteArray json;
    if (!serialize(config, &json, error)) return false;
    const int lock = SafeFs::lockFile(prefix, ".darling-launcher/lock", 0755, error);
    if (lock < 0) return false;
    struct Closer { int fd; ~Closer() { ::close(fd); } } closer{lock};
    QString why;
    if (PrefixState::prefixRunning(prefix, procFs, &why)) return fail(error, "Refusing to change the dyld cache settings while the prefix may be running: " + why);
    // policy first: a crash between the two leaves a config that load() accepts and a policy the launcher checks at launch
    const bool ok = wantsCache(config) ? SafeFs::writeFile(prefix, kPolicyRel, renderPolicy(config.served), 0644, 0755, true, error)
                                       : SafeFs::removeFile(prefix, kPolicyRel, error);
    return ok && SafeFs::writeFile(prefix, kConfigRel, json, 0644, 0755, true, error);
}
}
