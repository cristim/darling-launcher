// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "dyldcacheheader.h"
#include "discovery.h"
#include <QByteArray>
#include <QList>
#include <QString>
#include <functional>

// A user-private copy of the user's own macOS dyld shared cache, shared by Darling prefixes.
// <root>/<arch>-<uuid32>/ holds the cache files (named as on the source), written 0400, and store.json,
// which is written last: its presence means the copy is complete. The directory and the root are 0700.
// Store paths are private: nothing here puts a path in a message meant for display or for the MCP server.
struct StoreFile {
    QString name;
    qint64 size = 0;
    QString uuid;   // hex, from the file's own header
    QString sha256; // hex, of the local copy
};
struct StoreManifest {
    QString arch;
    QString uuid;       // hex UUID of the main file
    QString copiedAt;   // ISO date
    QList<StoreFile> files; // main file first
};
enum class StoreState {
    Missing,  // no directory
    Partial,  // directory without store.json
    Ready,    // manifest valid, every file has the recorded size and header UUID
    Corrupt,  // manifest invalid or a file differs from it
};
struct StoreStatus {
    StoreState state = StoreState::Missing;
    StoreManifest manifest; // set for Ready and Corrupt-with-manifest
    QString detail;         // human readable, no paths
};

namespace DyldCacheStore {
constexpr qint64 kDefaultMarginBytes = qint64(2) << 30;
constexpr int kMaxGuestDirChars = 200; // dyld rejects a longer SPLIT_DIR
extern const char *const kGuestHostRoot; // "/Volumes/SystemRoot"

QString defaultRoot();
// The location chosen in the launcher settings (key dyldCacheRoot), defaultRoot() when unset.
QString configuredRoot();
QString storeId(const QString &arch, const QString &uuidHex);
bool validStoreId(const QString &id);
// Creates the root (0700) when missing and refuses one that is not owned by this user or is open to group/others.
// Also refuses a root that is a symlink, and any directory between dataRoot and the root that is one.
bool ensureRoot(const QString &root, QString *error, const QString &dataRoot = LauncherDiscovery::dataRoot());
// The same checks without creating anything, plus the store directory itself: used before a launch trusts the store.
bool checkPrivate(const QString &root, const QString &id, QString *error, const QString &dataRoot = LauncherDiscovery::dataRoot());

bool parseManifest(const QByteArray &json, StoreManifest *out, QString *error);
QByteArray renderManifest(const StoreManifest &m);
bool saveManifest(const QString &root, const QString &id, const StoreManifest &m, QString *error);
// Quick check used at launch and in the UI: sizes and header UUIDs only, no hashing. A store whose UUID differs from
// the source cache's UUID is an older macOS build: still usable, and the UI compares the two.
StoreStatus status(const QString &root, const QString &id);
// Store ids found under root (directories only, never symlinks), sorted.
QStringList listStores(const QString &root);

// Reads up to `window` bytes from the start of root/relPath (no symlinks); *fileSize is the full length.
bool readHead(const QString &root, const QString &relPath, qint64 window, QByteArray *head, qint64 *fileSize, QString *error);

using FreeBytesFn = std::function<qint64(const QString &existingDir)>;
qint64 freeBytes(const QString &existingDir); // statvfs, -1 on failure
// Refuses unless at least needed + margin bytes are free on the filesystem that holds root (or its nearest existing ancestor).
bool checkFreeSpace(const QString &root, qint64 neededBytes, qint64 marginBytes, QString *error, const FreeBytesFn &fn = &freeBytes);

// The directory as the guest sees it: /Volumes/SystemRoot + canonical host path. Fails when it is not an existing
// directory or longer than dyld accepts.
bool guestDir(const QString &root, const QString &id, QString *guestPath, QString *error);
}
