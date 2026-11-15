// SPDX-License-Identifier: GPL-3.0-or-later
#include "appneedscache.h"
#include "appneeds.h"
#include "core.h"
#include "dyldcacheconfig.h"
#include "dyldcachesettings.h"
#include "launchenv.h"
#include "safefs.h"
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace AppNeedsCache {
namespace {
using LauncherCore::fail;
// The arm64e cache files, by name: the main file and its numbered sub-caches. Not .map, .atlas or the Rosetta and x86_64 files.
const QRegularExpression &cacheFileName() {
    static const QRegularExpression re(R"(^dyld_shared_cache_arm64e(\.[0-9]{2}(\.[A-Za-z]+)?)?$)");
    return re;
}
QString sizeText(qint64 bytes) { return DyldCacheSettings::formatBytes(bytes); }
}

QString findCacheVolume(const QString &appVolume, const QList<SourceMount> &mounts) {
    QStringList candidates{appVolume};
    for (const SourceMount &mount : mounts) candidates << mount.path << mount.path + "/root";
    for (const QString &candidate : candidates) {
        QString dir, ignored;
        if (!candidate.isEmpty() && DyldCacheSettings::resolveSourceDir(candidate, &dir, &ignored)) return candidate;
    }
    return {};
}

QString notice(qint64 totalBytes) {
    return QString("These files are Apple executable programs (the macOS dyld shared cache, about %1), copied bit for bit from the macOS "
                   "installation on this computer into a private folder that only your user can read. They stay on this machine, are never "
                   "committed, uploaded or redistributed by the launcher, and Apple's software licence applies to them: use them only as "
                   "the licence allows for your own Mac. The copy is checked against the source (size, header and a checksum computed over "
                   "the bytes the launcher copies) and is removed by deleting its folder.").arg(totalBytes > 0 ? sizeText(totalBytes) : QString("several GiB"));
}

bool buildPreview(const Request &request, const Options &options, Preview *preview, QString *error) {
    Preview p;
    p.marginBytes = options.marginBytes;
    QString note;
    if (!LaunchEnv::appRunsAsArm64e(request.volume, request.executableRel, &note)) {
        p.detail = note; *preview = p; return true;
    }
    int cacheServed = 0;
    for (const QString &name : request.linked)
        if ((name.startsWith("/System/Library/") || name.startsWith("/usr/lib/")) && !QFileInfo::exists(request.volume + name)) ++cacheServed;
    if (cacheServed == 0) { p.detail = "Every library the app links has a file on the volume, so none is served by the dyld cache."; *preview = p; return true; }
    p.cacheApp = true;
    p.detail = QString("The app runs as arm64e and links %1 libraries that have no file on the volume (they live in the dyld shared cache).").arg(cacheServed);
    for (const QString &name : request.linked)
        if (DyldCacheConfigFile::validatedLibraries().contains(name)) p.servedLibraries << name;

    QString dir, why;
    if (request.cacheVolume.isEmpty()) { p.blocker = "The volume that holds the dyld cache (the macOS cryptex OS image) is not mounted."; *preview = p; return true; }
    if (!DyldCacheSettings::resolveSourceDir(request.cacheVolume, &dir, &why)) { p.blocker = why; *preview = p; return true; }
    for (const QFileInfo &info : QDir(dir).entryInfoList(QDir::Files | QDir::NoSymLinks | QDir::NoDotAndDotDot, QDir::Name))
        if (cacheFileName().match(info.fileName()).hasMatch()) { p.files.append({info.fileName(), info.size()}); p.totalBytes += info.size(); }
    if (p.files.isEmpty() || p.files.first().name != "dyld_shared_cache_arm64e") {
        p.files.clear(); p.totalBytes = 0;
        p.blocker = "The macOS volume has no dyld_shared_cache_arm64e file (or it is a symlink)."; *preview = p; return true;
    }
    // already present: a complete store with the same file names and sizes
    for (const QString &id : DyldCacheStore::listStores(request.storeRoot)) {
        const StoreStatus st = DyldCacheStore::status(request.storeRoot, id);
        if (st.state != StoreState::Ready || st.manifest.arch != "arm64e" || st.manifest.files.size() != p.files.size()) continue;
        bool same = true;
        for (int i = 0; i < p.files.size() && same; ++i) same = st.manifest.files[i].name == p.files[i].name && st.manifest.files[i].size == p.files[i].size;
        if (same) { p.matchingStore = id; break; }
    }
    const DyldCacheStore::FreeBytesFn freeFn = options.freeBytes ? options.freeBytes : DyldCacheStore::FreeBytesFn(&DyldCacheStore::freeBytes);
    QFileInfo probe(request.storeRoot);
    while (!probe.exists() && probe.absolutePath() != probe.absoluteFilePath()) probe = QFileInfo(probe.absolutePath());
    p.freeBytes = freeFn(probe.absoluteFilePath());
    if (!p.alreadyPresent()) {
        QString space;
        if (!DyldCacheStore::checkFreeSpace(request.storeRoot, p.totalBytes, options.marginBytes, &space, freeFn)) p.blocker = space;
    }
    (void)error;
    *preview = p;
    return true;
}

QString summarize(const Preview &p) {
    QString text = "dyld shared cache: " + p.detail + "\n";
    if (!p.cacheApp) return text + "The cache step is not needed for this app.\n";
    for (const CacheFile &f : p.files) text += QString("  %1  %2\n").arg(f.name, sizeText(f.size));
    if (!p.files.isEmpty()) text += QString("Total: %1 in %2 files. Free space at the store: %3 (the copy keeps a %4 margin).\n")
                                        .arg(sizeText(p.totalBytes)).arg(p.files.size()).arg(p.freeBytes < 0 ? QString("unknown") : sizeText(p.freeBytes)).arg(sizeText(p.marginBytes));
    if (p.alreadyPresent()) text += "Already present and identical (same file names and sizes in a complete store): nothing to copy.\n";
    if (!p.blocker.isEmpty()) text += "Cannot be offered: " + p.blocker + "\n";
    text += p.servedLibraries.isEmpty()
        ? "After the copy this prefix's settings stay unchanged: no library this app links is validated to run from the cache (validated: " + DyldCacheConfigFile::validatedLibraries().join(", ") + ").\n"
        : "After the copy this prefix will serve from the cache: " + p.servedLibraries.join(", ") + ".\n";
    if (!p.alreadyPresent() && p.blocker.isEmpty()) text += "\n" + notice(p.totalBytes) + "\n";
    return text;
}

namespace {
// Removes the files and the directory a failed or cancelled copy left, only for a store this call created.
void removeStore(const QString &root, const QString &id) {
    if (!DyldCacheStore::validStoreId(id)) return;
    QString ignored;
    for (const QFileInfo &entry : QDir(root + "/" + id).entryInfoList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot))
        SafeFs::removeFile(root, id + "/" + entry.fileName(), &ignored);
    SafeFs::removeDirectory(root, id, &ignored);
}
}

bool apply(const Request &request, const Preview &preview, bool accepted, const Options &options, Applied *applied, QString *error,
           const DyldCacheCopy::ProgressFn &progress, const std::atomic<bool> *cancel) {
    *applied = {};
    if (!preview.cacheApp) return fail(error, "This app does not use the dyld cache; nothing was copied.");
    if (!preview.blocker.isEmpty()) return fail(error, preview.blocker);
    if (!accepted) return fail(error, "The Apple-binaries notice was not accepted: nothing was copied.");
    QString id = preview.matchingStore;
    if (id.isEmpty()) {
        QString dir, why;
        if (!DyldCacheSettings::resolveSourceDir(request.cacheVolume, &dir, &why)) return fail(error, why);
        const QStringList before = DyldCacheStore::listStores(request.storeRoot);
        DyldCacheCopy::Options copy = options.copy;
        copy.marginBytes = options.marginBytes;
        if (options.freeBytes) copy.freeBytes = options.freeBytes;
        const DyldCacheCopy::Outcome outcome = DyldCacheCopy::copyCache(dir, DyldCacheSettings::kArch, request.storeRoot, copy, progress, cancel);
        if (outcome.result != DyldCacheCopy::Result::Done) {
            if (!outcome.storeId.isEmpty() && !before.contains(outcome.storeId)) removeStore(request.storeRoot, outcome.storeId);
            return fail(error, outcome.result == DyldCacheCopy::Result::Cancelled ? "The cache copy was cancelled and the partial copy was removed." : outcome.error);
        }
        id = outcome.storeId; applied->copied = true;
    }
    applied->storeId = id;
    if (preview.servedLibraries.isEmpty()) {
        applied->pointerNote = "No library this app links is validated to run from the cache, so the prefix settings were not changed.";
    } else {
        DyldCacheConfig config;
        if (!DyldCacheConfigFile::load(request.prefix, &config, error)) return false;
        if (DyldCacheConfigFile::wantsCache(config) && config.store != id) {
            applied->pointerNote = "The prefix already uses another cache store; its settings were not changed (use dyld-cache prefix to switch).";
        } else {
            QString why;
            if (!DyldCacheStore::checkUsable(request.storeRoot, id, &why)) return fail(error, why);
            config.mode = DyldCacheMode::AllowList; config.store = id;
            for (const QString &name : preview.servedLibraries) if (!config.served.contains(name)) config.served << name;
            if (!DyldCacheConfigFile::save(request.prefix, config, error)) return false;
            applied->pointerWritten = true;
            applied->pointerNote = "The prefix now serves " + preview.servedLibraries.join(", ") + " from the cache for arm64e app launches.";
        }
    }
    QStringList names; for (const CacheFile &f : preview.files) names << f.name;
    return AppNeeds::appendLog(request.prefix, request.appName, "dyld cache", QString("%1 files").arg(names.size()),
                               QString("%1 bytes").arg(preview.totalBytes),
                               QString(applied->copied ? "copied" : "already present") + ", store " + id.left(14) + ", " + applied->pointerNote, error);
}
}
