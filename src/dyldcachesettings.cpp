// SPDX-License-Identifier: GPL-3.0-or-later
#include "dyldcachesettings.h"
#include "core.h"
#include "launchenv.h"
#include <QDir>
#include <QFileInfo>

namespace DyldCacheSettings {
using LauncherCore::fail;
const char *const kArch = "arm64e";

bool isInside(const QString &root, const QString &path) {
    return path == root || path.startsWith(root.endsWith('/') ? root : root + '/');
}

bool resolveSourceDir(const QString &volume, QString *dir, QString *error) {
    if (volume.isEmpty()) return fail(error, "No mounted macOS volume is selected");
    const QString root = QFileInfo(volume).canonicalFilePath();
    if (root.isEmpty() || !QFileInfo(root).isDir()) return fail(error, "The selected macOS volume is not a mounted directory");
    const QString canonical = QFileInfo(root + "/System/Library/dyld").canonicalFilePath();
    if (canonical.isEmpty() || !QFileInfo(canonical).isDir()) return fail(error, "The selected volume has no System/Library/dyld directory; mount the macOS cryptex OS image or a volume that holds the dyld cache");
    // a symlink inside a mounted volume must not lead the copy to files outside it
    if (canonical == root || !isInside(root, canonical)) return fail(error, "System/Library/dyld on the selected volume leads outside the volume");
    *dir = canonical;
    return true;
}

QString formatBytes(qint64 bytes) {
    if (bytes >= (qint64(1) << 30)) return QString::number(double(bytes) / double(qint64(1) << 30), 'f', 1) + " GiB";
    return QString::number(double(bytes) / double(qint64(1) << 20), 'f', 1) + " MiB";
}

QString progressText(const DyldCacheCopy::Progress &p) {
    return QString("file %1 of %2, %3 of %4").arg(p.fileIndex).arg(p.fileCount).arg(formatBytes(p.totalBytes), formatBytes(p.totalSize));
}

namespace {
const char *stateName(StoreState s) {
    switch (s) {
    case StoreState::Ready: return "Ready";
    case StoreState::Partial: return "Incomplete (Copy resumes it)";
    case StoreState::Corrupt: return "Damaged";
    case StoreState::Missing: break;
    }
    return "Missing";
}
QString shortId(const QString &id) { return id.size() > 14 ? id.left(14) + "..." : id; }
}

StoreOverview overview(const QString &storeRoot, const QString &sourceId, const QString &sourceError) {
    StoreOverview o;
    o.sourceKnown = !sourceId.isEmpty(); o.sourceId = sourceId;
    for (const QString &id : DyldCacheStore::listStores(storeRoot)) {
        const StoreStatus st = DyldCacheStore::status(storeRoot, id);
        StoreRow row; row.id = id; row.state = st.state;
        row.text = shortId(id) + ": " + stateName(st.state);
        if (st.state == StoreState::Ready) {
            qint64 bytes = 0;
            for (const StoreFile &f : st.manifest.files) bytes += f.size;
            row.text += QString(", %1, %2 files, %3, copied %4").arg(st.manifest.arch).arg(st.manifest.files.size()).arg(formatBytes(bytes), st.manifest.copiedAt);
        } else if (!st.detail.isEmpty()) row.text += ": " + st.detail;
        if (st.state == StoreState::Ready) o.readyIds << id;
        o.rows << row;
    }
    if (o.sourceKnown) o.stale = !o.readyIds.contains(sourceId);
    bool hasDamaged = false, started = false; // started: the source's own cache has a directory in the store
    for (const StoreRow &r : o.rows) { hasDamaged = hasDamaged || r.state == StoreState::Corrupt; started = started || r.id == sourceId; }
    if (!o.sourceKnown) {
        o.headline = (o.rows.isEmpty() ? QString("No private store yet. ") : QString()) + "The volume's cache could not be compared: " + sourceError;
    } else if (o.rows.isEmpty()) {
        o.headline = "No private store yet. Copy the cache from the mounted volume.";
    } else if (!o.stale) {
        o.headline = hasDamaged ? "The store for this volume is current. Another store is damaged; run Verify." : "The store is current: it matches the cache on the mounted volume.";
    } else {
        o.headline = started ? "STALE: the copy of the mounted volume's cache is not finished. Copy resumes it."
                             : "STALE: the mounted volume has a different cache (another macOS build) than the store. Copy adds the new one.";
    }
    return o;
}

QString enableBlocker(const QString &storeRoot, const QString &storeId) {
    if (storeId.isEmpty()) return "No ready store: copy the cache from the macOS volume first";
    QString error;
    return DyldCacheStore::checkUsable(storeRoot, storeId, &error) ? QString() : error;
}

PrefixStatus prefixStatus(const DyldCacheConfig &config, const QString &storeRoot) {
    PrefixStatus s;
    if (config.mode == DyldCacheMode::Off) { s.text = "Off: this prefix uses Darling's own libraries."; return s; }
    if (config.served.isEmpty()) { s.text = "Off in effect: no library is selected."; return s; }
    const QString blocker = enableBlocker(storeRoot, config.store);
    if (!blocker.isEmpty()) { s.text = "Not active: " + blocker + ". A launch of this prefix will refuse to start until this is fixed or the cache is turned off."; return s; }
    s.effective = true;
    s.text = QString("Active: %1 librar%2 served from store %3 to arm64e app launches.").arg(config.served.size()).arg(config.served.size() == 1 ? "y" : "ies", shortId(config.store));
    return s;
}

bool buildConfig(const DyldCacheConfig &previous, bool enabled, const QString &storeId, const QStringList &checked, DyldCacheConfig *out, QString *error) {
    DyldCacheConfig c = previous;
    c.mode = enabled ? DyldCacheMode::AllowList : DyldCacheMode::Off;
    if (!storeId.isEmpty()) c.store = storeId;
    // keep the validated list's order, so the file does not depend on click order
    c.served.clear();
    for (const QString &name : DyldCacheConfigFile::validatedLibraries()) if (checked.contains(name)) c.served << name;
    if (!DyldCacheConfigFile::validate(c, error)) return false;
    *out = c;
    return true;
}

QString launchRuleHelp() {
    return "The cache is given only to an app launch whose executable has exactly one arm64 slice and that slice is arm64e. "
           "Apps with a plain arm64 slice (alone or in a fat binary) and apps that are not arm64 run as before, and maintenance runs "
           "(check, repair, shutdown, shell, Brewfile) never get it. Turning it on also needs a Darling dyld that continues without a cache "
           "when the cache directory has no file for a process's architecture.";
}

QString appLine(const QString &prefix, const QString &appName, const QString &executableRel, const DyldCacheConfig &config) {
    if (!DyldCacheConfigFile::wantsCache(config)) return appName + " runs with Darling's libraries: the cache is not turned on for this prefix.";
    QString note;
    if (LaunchEnv::appRunsAsArm64e(prefix, executableRel, &note)) return appName + " is arm64e: its launch gets the cache.";
    return appName + ": " + note;
}
}
