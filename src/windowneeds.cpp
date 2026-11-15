// SPDX-License-Identifier: GPL-3.0-or-later
// The part of Window that works out and offers what an imported app needs: framework resources, the dyld cache, and the
// explained mount of the macOS volumes. Every copy waits for a dialog the user accepts.
#include "window.h"
#include "appneedsdialog.h"
#include "automount.h"
#include "automountstep.h"
#include "dyldcachestore.h"
#include "log.h"
#include <QFutureWatcher>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressDialog>
#include <QSettings>
#include <QStatusBar>
#include <QThread>
#include <QtConcurrent>
#include <atomic>
#include <memory>

namespace {
// The mounted volumes that hold apps, from the window's own mount table.
QStringList appSources(const QList<SourceMount> &mounts) {
    QStringList roots;
    for (const SourceCandidate &candidate : LauncherSources::candidates(mounts)) if (candidate.readable && candidate.apps) roots << candidate.root;
    return roots;
}
}

void Window::adoptMountedSource(const QString &path) {
    volume->setText(path); updateSourceChoices(); refresh(); setBusy(false, "Selected mounted macOS source: " + path);
    QSettings("cristim", "darling-launcher").setValue("volume", path);
}

void Window::offerAppNeeds(const QList<AppEntry> &imported, const QString &source, const QString &destination) {
    const QString storeRoot = cacheStoreRoot.isEmpty() ? DyldCacheStore::configuredRoot() : cacheStoreRoot;
    const QList<SourceMount> mounts = mountProvider();
    for (const AppEntry &entry : imported) {
        auto *watcher = new QFutureWatcher<AppNeedsAnalysis>(this);
        connect(watcher, &QFutureWatcher<AppNeedsAnalysis>::finished, this, [this, watcher, source, destination] {
            const AppNeedsAnalysis analysis = watcher->result(); watcher->deleteLater();
            showAppNeeds(analysis, source, destination);
        });
        const QString bundle = entry.sourceRelative;
        watcher->setFuture(QtConcurrent::run([=] {
            AppNeedsAnalysis a; a.appName = QFileInfo(bundle).completeBaseName(); QString error;
            if (!AppNeeds::buildPlan(source, destination, bundle, {}, &a.plan, &error)) { a.error = error; return a; }
            a.request = {a.plan.appName, source, AppNeedsCache::findCacheVolume(source, mounts), destination, storeRoot, a.plan.executableRel, a.plan.linkedInstallNames};
            if (!AppNeedsCache::buildPreview(a.request, {}, &a.cache, &error)) a.error = error;
            return a;
        }));
    }
}

void Window::showAppNeeds(const AppNeedsAnalysis &analysis, const QString &source, const QString &destination) {
    if (!analysis.error.isEmpty()) { LauncherLog::write("appneeds", analysis.appName + ": could not work out its frameworks: " + analysis.error); return; }
    const AppNeedsCache::Request request = analysis.request;
    const bool offerCacheToo = analysis.cache.cacheApp && !analysis.cache.alreadyPresent();
    if (analysis.plan.selectedCount() + analysis.plan.byStatus(AppNeeds::Status::Optional).size() == 0) {
        statusBar()->showMessage(analysis.appName + " needs no framework resources that are missing from the prefix.");
        if (offerCacheToo) offerCache(request);
        return;
    }
    auto *dialog = new AppNeedsDialog(analysis.plan, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::accepted, this, [this, dialog, source, destination, request, offerCacheToo] {
        const AppNeeds::Plan chosen = dialog->plan();
        auto *watcher = new QFutureWatcher<QPair<AppNeeds::Applied, QString>>(this);
        setBusy(true, "Copying framework resources…");
        connect(watcher, &QFutureWatcher<QPair<AppNeeds::Applied, QString>>::finished, this, [this, watcher, request, offerCacheToo] {
            const auto result = watcher->result(); watcher->deleteLater();
            const QString message = !result.second.isEmpty() ? result.second
                : QString("Copied framework resources for %1 bundles (%2 files, %3).").arg(result.first.bundles.size()).arg(result.first.result.files).arg(FrameworkResources::formatBytes(result.first.result.bytes));
            LauncherLog::write("appneeds", message);
            setBusy(activeProcesses > 0, message);
            if (!result.second.isEmpty()) QMessageBox::warning(this, "Framework resources", message);
            else if (offerCacheToo) offerCache(request);
        });
        watcher->setFuture(QtConcurrent::run([source, destination, chosen] {
            AppNeeds::Applied applied; QString error;
            AppNeeds::applyResources(source, destination, chosen, true, {}, &applied, &error);
            return qMakePair(applied, error);
        }));
    });
    connect(dialog, &QDialog::rejected, this, [this, request, offerCacheToo] {
        statusBar()->showMessage("No framework resources were copied.");
        if (offerCacheToo) offerCache(request);
    });
    dialog->open();
}

void Window::offerCache(const AppNeedsCache::Request &request) {
    const QList<SourceMount> mounts = mountProvider();
    auto *watcher = new QFutureWatcher<QPair<AppNeedsCache::Request, AppNeedsCache::Preview>>(this);
    connect(watcher, &QFutureWatcher<QPair<AppNeedsCache::Request, AppNeedsCache::Preview>>::finished, this, [this, watcher] {
        const auto result = watcher->result(); watcher->deleteLater();
        showCache(result.first, result.second);
    });
    watcher->setFuture(QtConcurrent::run([request, mounts] {
        AppNeedsCache::Request fresh = request; QString error; AppNeedsCache::Preview preview;
        fresh.cacheVolume = AppNeedsCache::findCacheVolume(request.volume, mounts); // the cryptex image may have been mounted since
        AppNeedsCache::buildPreview(fresh, {}, &preview, &error);
        return qMakePair(fresh, preview);
    }));
}

void Window::showCache(const AppNeedsCache::Request &request, const AppNeedsCache::Preview &preview) {
    if (!preview.cacheApp) return;
    if (preview.alreadyPresent()) { statusBar()->showMessage("The dyld cache is already present in the private store; nothing to copy."); return; }
    auto *dialog = new CacheOfferDialog(preview, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &CacheOfferDialog::mountRequested, this, [this, dialog, request] {
        dialog->reject();
        runAutoMount(true, [this, request] { offerCache(request); });
    });
    connect(dialog, &QDialog::accepted, this, [this, request, preview] {
        auto *bar = new QProgressDialog("Copying the dyld cache into the private store…", "Cancel", 0, 100, this);
        bar->setObjectName("cacheProgress"); bar->setAttribute(Qt::WA_DeleteOnClose); bar->setMinimumDuration(0); bar->setAutoClose(false); bar->setAutoReset(false);
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        connect(bar, &QProgressDialog::canceled, this, [cancel] { cancel->store(true); });
        auto *watcher = new QFutureWatcher<QPair<AppNeedsCache::Applied, QString>>(this);
        connect(watcher, &QFutureWatcher<QPair<AppNeedsCache::Applied, QString>>::finished, this, [this, watcher, bar] {
            const auto result = watcher->result(); watcher->deleteLater(); bar->close();
            const QString message = !result.second.isEmpty() ? result.second
                : (result.first.copied ? "Copied the dyld cache into the private store. " : "The dyld cache was already present. ") + result.first.pointerNote;
            LauncherLog::write("appneeds", message);
            setBusy(activeProcesses > 0, message);
            if (!result.second.isEmpty()) QMessageBox::warning(this, "dyld shared cache", message);
        });
        QPointer<QProgressDialog> guard(bar);
        watcher->setFuture(QtConcurrent::run([request, preview, cancel, guard] {
            AppNeedsCache::Applied applied; QString error;
            AppNeedsCache::apply(request, preview, true, {}, &applied, &error, [guard](const DyldCacheCopy::Progress &p) {
                const int percent = p.totalSize > 0 ? int(p.totalBytes * 100 / p.totalSize) : 0;
                QMetaObject::invokeMethod(guard.data(), [guard, percent] { if (guard) guard->setValue(percent); }, Qt::QueuedConnection);
            }, cancel.get());
            return qMakePair(applied, error);
        }));
    });
    dialog->open();
}

void Window::runAutoMount(bool wantCache, std::function<void()> afterMounted) {
    if (importRunning) { statusBar()->showMessage("An import is in progress; mount the volumes when it has finished."); return; }
    const auto provider = partitionProvider ? partitionProvider : std::function<QList<MacPartition>(QString *)>(&AutoMount::discoverPartitions);
    const QList<SourceMount> mountsNow = mountProvider();
    if (!AutoMount::needed(mountsNow, wantCache).any()) {
        statusBar()->showMessage("The macOS volumes are already mounted and readable.");
        if (volume->text().isEmpty()) { const QStringList sources = appSources(mountProvider()); if (sources.size() == 1) adoptMountedSource(sources.first()); }
        if (afterMounted) afterMounted();
        return;
    }
    auto *scan = new QFutureWatcher<QPair<QList<MacPartition>, QString>>(this);
    setBusy(true, "Looking for the macOS partition…");
    connect(scan, &QFutureWatcher<QPair<QList<MacPartition>, QString>>::finished, this, [this, scan, wantCache, afterMounted] {
        const auto found = scan->result(); scan->deleteLater();
        setBusy(activeProcesses > 0, found.first.isEmpty() ? "No macOS partition was found." : "Waiting for your decision about mounting the macOS volumes.");
        if (found.first.isEmpty()) {
            QMessageBox::warning(this, "Mount macOS volumes", (found.second.isEmpty() ? QString("No APFS partition was found on this computer.") : found.second) + "\n\n" + AutoMount::manualInstructions());
            return;
        }
        const AutoMount::Need need = AutoMount::needed(mountProvider(), wantCache);
        auto *dialog = new MountExplainDialog(need, found.first, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(dialog, &QDialog::rejected, this, [this] { setBusy(activeProcesses > 0, "Nothing was mounted. " + AutoMount::manualInstructions()); });
        connect(dialog, &QDialog::accepted, this, [this, dialog, wantCache, afterMounted] {
            const MacPartition chosen = dialog->chosen();
            const std::shared_ptr<AutoMount::Step> step = autoMountStep ? std::shared_ptr<AutoMount::Step>(autoMountStep, [](AutoMount::Step *) {}) : std::make_shared<HelperMountStep>();
            const auto provider = mountProvider;
            auto *watcher = new QFutureWatcher<AutoMount::Outcome>(this);
            setBusy(true, "Mounting the macOS volumes read-only… (authenticate in the system dialog)");
            connect(watcher, &QFutureWatcher<AutoMount::Outcome>::finished, this, [this, watcher, afterMounted] {
                const AutoMount::Outcome outcome = watcher->result(); watcher->deleteLater();
                LauncherLog::write("automount", outcome.message + " steps: " + outcome.stepsRun.join(", "));
                setBusy(activeProcesses > 0, outcome.message);
                using State = AutoMount::Outcome::State;
                if (outcome.state == State::Failed) { QMessageBox::warning(this, "Mount macOS volumes", outcome.message + "\n\n" + AutoMount::manualInstructions()); return; }
                if (outcome.state == State::Cancelled) return;
                const QStringList sources = appSources(mountProvider());
                if (volume->text().isEmpty() && sources.size() == 1) adoptMountedSource(sources.first());
                else refresh();
                if (afterMounted) afterMounted();
            });
            watcher->setFuture(QtConcurrent::run([wantCache, chosen, step, provider] {
                AutoMount::Hooks hooks;
                hooks.mounts = provider; hooks.consent = [](const QString &) { return true; }; // the dialog showed this very text
                hooks.sleep = [](int ms) { QThread::msleep(ulong(ms)); };
                return AutoMount::ensureMounted(wantCache, chosen, *step, hooks);
            }));
        });
        dialog->open();
    });
    scan->setFuture(QtConcurrent::run([provider] { QString error; const QList<MacPartition> parts = provider(&error); return qMakePair(parts, error); }));
}
