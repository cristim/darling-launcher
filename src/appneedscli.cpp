// SPDX-License-Identifier: GPL-3.0-or-later
#include "appneedscli.h"
#include "appneeds.h"
#include "appneedscache.h"
#include "core.h"
#include "dyldcachesettings.h"
#include <QDir>
#include <QFileInfo>
#include <QThread>
#include <cstdio>
#include <unistd.h>

namespace {
const QString kUsage = "usage: darling-launcher app-import --prefix DIR --app REL [--volume DIR] [--also NAME[,NAME...]] "
                       "[--with-cache [--cache-volume DIR] [--accept-apple-binaries]] (--dry-run | --yes)";
struct Args {
    QString volume, prefix, app, also, cacheVolume;
    bool dryRun = false, yes = false, withCache = false, accept = false;
};
bool parse(const QStringList &args, Args *a) {
    for (int i = 0; i < args.size(); ++i) {
        if (args[i] == "--dry-run" && !a->dryRun) { a->dryRun = true; continue; }
        if (args[i] == "--yes" && !a->yes) { a->yes = true; continue; }
        if (args[i] == "--with-cache" && !a->withCache) { a->withCache = true; continue; }
        if (args[i] == "--accept-apple-binaries" && !a->accept) { a->accept = true; continue; }
        QString *target = args[i] == "--volume" ? &a->volume : args[i] == "--prefix" ? &a->prefix : args[i] == "--app" ? &a->app
                          : args[i] == "--also" ? &a->also : args[i] == "--cache-volume" ? &a->cacheVolume : nullptr;
        if (!target || i + 1 >= args.size() || args[i + 1].isEmpty() || !target->isEmpty()) return false;
        *target = args[++i];
    }
    return !a->prefix.isEmpty() && !a->app.isEmpty() && !(a->dryRun && a->yes) && !(a->accept && !a->withCache) && !(a->withCache && !a->dryRun && !a->yes);
}

// Finds the mounted source, running the explained mount flow when there is none. Returns false with *message set.
bool ensureSource(const AppImportEnv &env, bool wantCache, QTextStream &out, QString *message) {
    AutoMount::Hooks hooks;
    hooks.mounts = env.mounts ? env.mounts : std::function<QList<SourceMount>()>(&LauncherSources::mounts);
    hooks.sleep = env.sleep ? env.sleep : std::function<void(int)>([](int ms) { QThread::msleep(ulong(ms)); });
    if (!AutoMount::needed(hooks.mounts(), wantCache).any()) return true;
    const bool interactive = env.interactive ? env.interactive() : ::isatty(0);
    if (!env.mountStep || !interactive) { *message = "The macOS volumes are not mounted. " + AutoMount::manualInstructions(); return false; }
    QString error;
    const QList<MacPartition> partitions = env.partitions ? env.partitions(&error) : AutoMount::discoverPartitions(&error);
    if (partitions.isEmpty()) { *message = error.isEmpty() ? "No APFS partition was found, so the macOS volumes cannot be mounted." : error; return false; }
    const auto readLine = [&]() -> QString { if (env.readLine) return env.readLine(); char buffer[256]; return std::fgets(buffer, sizeof(buffer), stdin) ? QString::fromLocal8Bit(buffer).trimmed() : QString(); };
    MacPartition chosen = partitions.first();
    if (partitions.size() > 1) {
        out << "Several APFS partitions were found. Which one holds macOS?\n";
        for (int i = 0; i < partitions.size(); ++i) out << QString("  %1) %2\n").arg(i + 1).arg(AutoMount::partitionLine(partitions[i]));
        out << "Number: " << Qt::flush;
        bool ok = false; const int number = readLine().toInt(&ok);
        if (!ok || number < 1 || number > partitions.size()) { *message = "No partition was chosen. " + AutoMount::manualInstructions(); return false; }
        chosen = partitions[number - 1];
    }
    hooks.consent = [&](const QString &text) { out << text << "\n\nContinue? [y/N] " << Qt::flush; const QString answer = readLine().toLower(); return answer == "y" || answer == "yes"; };
    const AutoMount::Outcome outcome = AutoMount::ensureMounted(wantCache, chosen, *env.mountStep, hooks);
    out << outcome.message << Qt::endl;
    if (outcome.state == AutoMount::Outcome::State::Mounted || outcome.state == AutoMount::Outcome::State::AlreadyMounted) return true;
    *message = outcome.message;
    return false;
}
}

int appImportMain(const QStringList &args, QTextStream &out, QTextStream &err, const AppImportEnv &env) {
    if (args == QStringList{"--help"}) { out << kUsage << Qt::endl; return 0; }
    Args a;
    if (!parse(args, &a)) { err << kUsage << Qt::endl; return 2; }
    QStringList also;
    if (!a.also.isEmpty()) {
        for (const QString &name : a.also.split(',')) also << name.trimmed();
        if (also.contains("")) { err << kUsage << Qt::endl; return 2; }
        also.removeDuplicates();
    }
    AppNeedsCache::Options cacheOptions;
    AppNeeds::Options options; options.also = also;
    if (env.freeBytesOverride >= 0) {
        options.freeBytes = [&](const QString &) { return env.freeBytesOverride; };
        cacheOptions.freeBytes = [&](const QString &) { return env.freeBytesOverride; };
    }
    QString volume = a.volume, message;
    if (volume.isEmpty()) {
        if (!ensureSource(env, false, out, &message)) { err << message << Qt::endl; return 1; }
        const auto mounts = env.mounts ? env.mounts() : LauncherSources::mounts();
        QStringList usable;
        for (const SourceCandidate &candidate : LauncherSources::candidates(mounts)) if (candidate.readable && candidate.apps) usable << candidate.root;
        if (usable.size() != 1) { err << (usable.isEmpty() ? "No usable mounted macOS source was found." : "Several mounted macOS sources were found; choose one with --volume.") << Qt::endl; return 1; }
        volume = usable.first();
    }
    const QString storeRoot = env.storeRoot.isEmpty() ? DyldCacheStore::configuredRoot() : env.storeRoot;
    AppNeeds::Plan plan; QString error;
    if (!AppNeeds::buildPlan(volume, a.prefix, a.app, options, &plan, &error)) { err << error << Qt::endl; return 1; }
    out << AppNeeds::summarize(plan);
    if (a.yes && plan.availableBytes >= 0 && qint64(plan.selectedBytes()) + FrameworkResources::Plan::kReserveBytes > plan.availableBytes) {
        err << "Not enough free space: the selected resources need about " << FrameworkResources::formatBytes(plan.selectedBytes()) << " plus a 512 MiB reserve. Nothing was copied." << Qt::endl;
        return 1;
    }

    const auto previewCache = [&](AppNeedsCache::Request *request, AppNeedsCache::Preview *preview) {
        const auto mounts = env.mounts ? env.mounts() : LauncherSources::mounts();
        *request = {plan.appName, volume, a.cacheVolume.isEmpty() ? AppNeedsCache::findCacheVolume(volume, mounts) : a.cacheVolume, a.prefix, storeRoot, plan.executableRel, plan.linkedInstallNames};
        return AppNeedsCache::buildPreview(*request, cacheOptions, preview, &error);
    };
    AppNeedsCache::Request cacheRequest; AppNeedsCache::Preview cache;
    if (!previewCache(&cacheRequest, &cache)) { err << error << Qt::endl; return 1; }
    out << AppNeedsCache::summarize(cache);
    if (cache.cacheApp) out << "To copy the cache too: re-run with --with-cache --accept-apple-binaries (a separate step; --yes alone never copies it).\n";
    // the cache step's own confirmation comes before anything is copied, so a refusal leaves nothing half done
    if (a.withCache && a.yes && !a.accept && cache.cacheApp) {
        err << AppNeedsCache::notice(cache.totalBytes) << "\nNothing was copied. Add --accept-apple-binaries to confirm the dyld cache copy." << Qt::endl;
        return 2;
    }
    if (a.dryRun) { out << "Dry run: nothing was copied." << Qt::endl; return 0; }
    if (!a.yes) { err << "Nothing was copied. Re-run with --yes to import " << FrameworkResources::formatBytes(plan.selectedBytes()) << " of selected resources, or with --dry-run to only see the plan." << Qt::endl; return 2; }

    const QString existing = a.prefix + "/Applications/" + QFileInfo(a.app).fileName();
    if (QFileInfo::exists(existing)) out << "The app is already in the prefix; not copied again.\n";
    else {
        AppEntry entry;
        if (!LauncherCore::importApp(volume, a.prefix, a.app, &entry, &error) || !LauncherCore::catalogImportedApp(a.prefix, entry, &error)) { err << error << Qt::endl; return 1; }
        out << "Imported the app into the prefix.\n";
    }
    AppNeeds::Applied applied;
    if (!AppNeeds::applyResources(volume, a.prefix, plan, true, options, &applied, &error)) { err << error << Qt::endl; return 1; }
    out << QString("Framework resources: %1 bundles, %2 files (%3), %4 directories, %5 symlinks.\n").arg(applied.bundles.size()).arg(applied.result.files)
               .arg(FrameworkResources::formatBytes(applied.result.bytes)).arg(applied.result.directories).arg(applied.result.symlinks);

    if (!a.withCache) return 0;
    if (!cache.cacheApp) { out << "The dyld cache step is not needed for this app.\n"; return 0; }
    if (!cache.blocker.isEmpty() && cache.files.isEmpty() && a.cacheVolume.isEmpty()) {
        if (!ensureSource(env, true, out, &message)) { err << message << Qt::endl; return 1; }
        if (!previewCache(&cacheRequest, &cache)) { err << error << Qt::endl; return 1; }
    }
    if (!cache.blocker.isEmpty()) { err << "The dyld cache step cannot run: " << cache.blocker << Qt::endl; return 1; }
    out << AppNeedsCache::notice(cache.totalBytes) << "\n";
    AppNeedsCache::Applied copied;
    int lastPercent = -1;
    const auto progress = [&](const DyldCacheCopy::Progress &p) {
        const int percent = p.totalSize > 0 ? int(p.totalBytes * 100 / p.totalSize) : 0;
        if (percent != lastPercent && percent % 5 == 0) { lastPercent = percent; err << QString("cache copy: %1%").arg(percent) << Qt::endl; }
    };
    if (!AppNeedsCache::apply(cacheRequest, cache, true, cacheOptions, &copied, &error, progress)) { err << error << Qt::endl; return 1; }
    out << (copied.copied ? "Copied the dyld cache into the private store. " : "The dyld cache was already present. ") << copied.pointerNote << Qt::endl;
    return 0;
}
