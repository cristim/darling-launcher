// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "dyldcacheconfig.h"
#include "dyldcachecopy.h"
#include "dyldcachestore.h"
#include <QList>
#include <QString>
#include <QStringList>

// The decisions behind the dyld cache page of the launcher settings, kept out of the widgets so they can be tested
// without a display. Nothing here writes anything; the page calls DyldCacheCopy and DyldCacheConfigFile for that.
namespace DyldCacheSettings {
// A slow FUSE daemon behind the mounted volume is easy to starve, so the page copies in small windows with a pause.
// (The CLI defaults to 128 MiB and no pause; a user who wants that runs the CLI.)
constexpr qint64 kWindowBytes = qint64(32) << 20;
constexpr int kThrottleMs = 300;
DyldCacheCopy::Options copyOptions();

// The architecture of the cache the page manages: the only one an arm64e app launch can use.
extern const char *const kArch;

// The directory holding the cache files on the selected macOS volume: <volume>/System/Library/dyld, resolved and
// required to stay inside the volume. Fails with a sentence for the user when it is missing or leaves the volume.
bool resolveSourceDir(const QString &volume, QString *dir, QString *error);

struct StoreRow {
    QString id;
    StoreState state = StoreState::Missing;
    QString text; // one line, no store path
};
struct StoreOverview {
    QList<StoreRow> rows;
    QStringList readyIds;
    bool sourceKnown = false;
    bool stale = false;   // the source's cache is not stored completely: an older macOS build is in the store, or none
    QString sourceId;
    QString headline;     // what the user should read first
};
// `sourceId` is empty when the source could not be read, and `sourceError` then says why.
StoreOverview overview(const QString &storeRoot, const QString &sourceId, const QString &sourceError);

QString formatBytes(qint64 bytes);
QString progressText(const DyldCacheCopy::Progress &p);

// What a prefix's settings amount to, as the launch will see them.
struct PrefixStatus {
    bool effective = false;
    QString text;
};
PrefixStatus prefixStatus(const DyldCacheConfig &config, const QString &storeRoot);
// Why the enable box has to stay disabled (empty when it may be ticked): the store the prefix would pin is not usable.
QString enableBlocker(const QString &storeRoot, const QString &storeId);
// The config the page writes for the box states. Off keeps the library selection, as the file format allows.
bool buildConfig(const DyldCacheConfig &previous, bool enabled, const QString &storeId, const QStringList &checked, DyldCacheConfig *out, QString *error);

// The launch rule shown beside the settings, and what it means for one app of the prefix (executable relative to it).
QString launchRuleHelp();
QString appLine(const QString &prefix, const QString &appName, const QString &executableRel, const DyldCacheConfig &config);
}
