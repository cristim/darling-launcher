// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "dyldcachestore.h"
#include <QString>
#include <atomic>
#include <functional>

// Copies the user's own split dyld cache from a directory (normally on the mounted macOS volume) into the private
// store, as the user: one file at a time, in throttled windows so a slow FUSE daemon is not starved, resumable
// after a crash or cancel, and renamed into place only when size and header UUID match the source.
// No root, no mount handling: the caller chooses the source directory.
namespace DyldCacheCopy {
struct Options {
    qint64 windowBytes = qint64(128) << 20; // read this much, flush, then pause
    qint64 blockBytes = qint64(1) << 20;    // one read; progress and cancel are checked per block
    int pauseMs = 0;                        // pause after each window
    qint64 marginBytes = DyldCacheStore::kDefaultMarginBytes;
    QString buildLabel;                     // recorded in the manifest; informational
    // Test seams.
    DyldCacheStore::FreeBytesFn freeBytes;                                   // default: statvfs
    std::function<void(int ms)> sleepFn;                                     // default: QThread::msleep
    std::function<void(const QString &file, qint64 offset, qint64 length)> onWindow; // called before each window is read
};
struct Progress {
    int fileIndex = 0, fileCount = 0;
    QString fileName;
    qint64 fileBytes = 0, fileSize = 0, totalBytes = 0, totalSize = 0;
};
using ProgressFn = std::function<void(const Progress &)>;

enum class Result { Done, Cancelled, Failed };
struct Outcome {
    Result result = Result::Failed;
    QString error;
    QString storeId; // set on Done, and on Cancelled once the store directory is known
};

// `arch` is arm64e or arm64 and selects dyld_shared_cache_<arch> in sourceDir. A store that is already Ready is
// reported as Done without reading the source files again. Never replaces or deletes an existing store file.
Outcome copyCache(const QString &sourceDir, const QString &arch, const QString &storeRoot, const Options &options = {},
                  const ProgressFn &progress = {}, const std::atomic<bool> *cancel = nullptr);
}
