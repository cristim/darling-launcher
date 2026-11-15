// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "dyldcachecopy.h"
#include "dyldcachestore.h"
#include "sources.h"
#include <QList>
#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>

// The dyld shared cache step of an app import. It is separate from the framework resource step on purpose: the cache is
// Apple executable code, so it is detected and previewed automatically but copied only after its own confirmation
// (--with-cache --accept-apple-binaries on the command line, the cache dialog in the GUI), never as part of the resource
// import's --yes.
//
// "Copy into the managed prefix" means what the existing managed prefixes do: the cache files are copied bit for bit,
// by DyldCacheCopy, into the launcher's private store (a 0700 folder, files 0400), and the prefix records which store its
// guest runs use (dyld-cache.json plus the policy file); the guest finds the folder through DYLD_SHARED_CACHE_SPLIT_DIR.
// The preview and the detection read names, sizes and presence only. Copying reuses the engine, which checks each copy
// against the source (size, header UUID, then a checksum of the copied bytes).
namespace AppNeedsCache {
struct CacheFile { QString name; qint64 size = 0; };
struct Preview {
    bool cacheApp = false;
    QString detail;                 // why the app is or is not a cache app
    QList<CacheFile> files;         // main file first, then the sub-caches, by name
    qint64 totalBytes = 0;
    QString matchingStore;          // a complete store whose file names and sizes equal the source's: already present
    QStringList servedLibraries;    // libraries the app links that are validated to run from the cache
    qint64 freeBytes = -1, marginBytes = 0;
    QString blocker;                // non-empty: the step cannot be offered, and why
    bool alreadyPresent() const { return !matchingStore.isEmpty(); }
};
struct Request {
    QString appName;
    QString volume;        // the volume the app was imported from
    QString cacheVolume;   // the mounted volume that holds System/Library/dyld (the cryptex OS image); may equal `volume`
    QString prefix;
    QString storeRoot;
    QString executableRel; // below `volume`
    QStringList linked;    // absolute install names the app links
};
struct Options {
    DyldCacheStore::FreeBytesFn freeBytes;                  // test seam; default statvfs
    qint64 marginBytes = DyldCacheStore::kDefaultMarginBytes;
    DyldCacheCopy::Options copy;                            // test seam: window, pause, free space
};

// The mount among `mounts` (or `appVolume` itself) whose System/Library/dyld exists; empty when none.
QString findCacheVolume(const QString &appVolume, const QList<SourceMount> &mounts);

bool buildPreview(const Request &request, const Options &options, Preview *preview, QString *error);
QString summarize(const Preview &preview);
// The sentence block the user must read before the copy; the dialog and the CLI show exactly this.
QString notice(qint64 totalBytes);

struct Applied {
    QString storeId;
    bool copied = false;          // false: the store was already present
    bool pointerWritten = false;
    QString pointerNote;
};
// Refuses unless `accepted`. A complete matching store is reused. A copy that fails or is cancelled and that this call
// started from nothing is removed again; a store that was already partial keeps its resumable state. Appends one row to
// the prefix's app-imports.md.
bool apply(const Request &request, const Preview &preview, bool accepted, const Options &options, Applied *applied, QString *error,
           const DyldCacheCopy::ProgressFn &progress = {}, const std::atomic<bool> *cancel = nullptr);
}
