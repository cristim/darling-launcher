// SPDX-License-Identifier: GPL-3.0-or-later
#include "launchenv.h"
#include "core.h"
#include "dyldcacheconfig.h"
#include "dyldcachestore.h"
#include "safefs.h"

namespace LaunchEnv {
const char *const kSplitDirVar = "DYLD_SHARED_CACHE_SPLIT_DIR";
const char *const kPolicyVar = "DYLD_SHARED_CACHE_SPLIT_POLICY";

void stripDyld(QProcessEnvironment *env) {
    for (const QString &name : env->keys()) if (name.startsWith("DYLD_")) env->remove(name);
}

bool guestEnvironment(QProcessEnvironment *env, const QString &prefix, const QString &storeRoot, QString *error) {
    using LauncherCore::fail;
    QString splitDir;
    if (!prefix.isEmpty()) {
        DyldCacheConfig config;
        if (!DyldCacheConfigFile::load(prefix, &config, error)) return false;
        if (DyldCacheConfigFile::wantsCache(config)) {
            const auto st = DyldCacheStore::status(storeRoot, config.store);
            if (st.state != StoreState::Ready && st.state != StoreState::Stale)
                return fail(error, "The dyld cache store for this prefix is not usable (" + (st.detail.isEmpty() ? QStringLiteral("not copied yet") : st.detail) + "). Copy it again or turn the cache off for this prefix.");
            QByteArray policy; bool exists = false;
            if (!SafeFs::readFile(prefix, ".darling-launcher/dyld-cache.policy", 256 * 1024, &policy, &exists, error)) return false;
            if (!exists || policy != DyldCacheConfigFile::renderPolicy(config.served))
                return fail(error, "The dyld cache policy file does not match the prefix settings. Save the dyld cache settings for this prefix again.");
            if (!DyldCacheStore::guestDir(storeRoot, config.store, &splitDir, error)) return false;
        }
    }
    stripDyld(env);
    if (!splitDir.isEmpty()) {
        env->insert(kSplitDirVar, splitDir);
        env->insert(kPolicyVar, DyldCacheConfigFile::kGuestPolicyPath);
    }
    return true;
}
}
