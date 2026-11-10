// SPDX-License-Identifier: GPL-3.0-or-later
#include "dyldcachedialog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtConcurrent>

using namespace DyldCacheSettings;

DyldCacheDialog::DyldCacheDialog(const QString &volume, const QString &prefix, const QString &storeRoot, const App &app, QWidget *parent)
    : QDialog(parent), volume(volume), prefix(prefix), storeRoot(storeRoot), app(app) {
    setObjectName("dyldCacheDialog"); setWindowTitle("dyld shared cache"); resize(760, 620);
    showError = [this](const QString &title, const QString &text) { QMessageBox::warning(this, title, text); };
    auto *layout = new QVBoxLayout(this);

    auto *store = new QGroupBox("Private cache store (shared by all prefixes)"); store->setObjectName("storeGroup");
    auto *storeLayout = new QVBoxLayout(store);
    location = new QLineEdit(storeRoot); location->setReadOnly(true); location->setObjectName("storeLocation");
    location->setToolTip("The location is a launcher setting (dyldCacheRoot); it is shown here, not edited.");
    auto *locationRow = new QHBoxLayout; locationRow->addWidget(new QLabel("Store location")); locationRow->addWidget(location); storeLayout->addLayout(locationRow);
    sourceLabel = new QLabel; sourceLabel->setObjectName("sourceLabel"); sourceLabel->setWordWrap(true); sourceLabel->setTextInteractionFlags(Qt::TextSelectableByMouse); storeLayout->addWidget(sourceLabel);
    headline = new QLabel; headline->setObjectName("storeHeadline"); headline->setWordWrap(true); storeLayout->addWidget(headline);
    rows = new QLabel; rows->setObjectName("storeRows"); rows->setWordWrap(true); rows->setTextInteractionFlags(Qt::TextSelectableByMouse); storeLayout->addWidget(rows);
    bar = new QProgressBar; bar->setObjectName("copyProgress"); bar->setRange(0, 1000); bar->setVisible(false); storeLayout->addWidget(bar);
    progressLabel = new QLabel; progressLabel->setObjectName("progressLabel"); progressLabel->setWordWrap(true); storeLayout->addWidget(progressLabel);
    auto *buttons = new QHBoxLayout;
    copyButton = new QPushButton("Copy / Refresh from volume"); copyButton->setObjectName("copyStore");
    copyButton->setToolTip("Copies the dyld cache of the selected macOS volume into the private store, in the background (small windows with a short pause, so a slow mount is not starved). A stopped copy resumes where it left off.");
    cancelButton = new QPushButton("Cancel"); cancelButton->setObjectName("cancelJob"); cancelButton->setToolTip("Stops cleanly; the partial copy is kept and Copy resumes it");
    verifyButton = new QPushButton("Verify"); verifyButton->setObjectName("verifyStore"); verifyButton->setToolTip("Re-checks sizes, headers and hashes of the local store; never reads the volume");
    refreshButton = new QPushButton("Re-read"); refreshButton->setObjectName("rereadStore"); refreshButton->setToolTip("Looks at the volume and the store again");
    for (auto *b : {copyButton, cancelButton, verifyButton, refreshButton}) buttons->addWidget(b);
    buttons->addStretch(); storeLayout->addLayout(buttons);
    layout->addWidget(store);

    prefixGroup = new QGroupBox("This prefix"); prefixGroup->setObjectName("prefixGroup");
    auto *prefixLayout = new QVBoxLayout(prefixGroup);
    auto *help = new QLabel(launchRuleHelp()); help->setObjectName("launchRuleHelp"); help->setWordWrap(true); prefixLayout->addWidget(help);
    enable = new QCheckBox("Serve the selected libraries from the dyld cache in this prefix"); enable->setObjectName("enableCache");
    prefixLayout->addWidget(enable);
    auto *storeRow = new QHBoxLayout; storeRow->addWidget(new QLabel("Store")); storeChoice = new QComboBox; storeChoice->setObjectName("storeChoice"); storeRow->addWidget(storeChoice, 1); prefixLayout->addLayout(storeRow);
    for (const QString &name : DyldCacheConfigFile::validatedLibraries()) {
        auto *box = new QCheckBox(name); box->setProperty("installName", name);
        box->setToolTip("Only libraries whose cache copy has been shown to run under Darling's dyld are listed. Darling's own copy is used unless this is ticked.");
        libraries << box; prefixLayout->addWidget(box);
    }
    prefixStatusLabel = new QLabel; prefixStatusLabel->setObjectName("prefixStatus"); prefixStatusLabel->setWordWrap(true); prefixLayout->addWidget(prefixStatusLabel);
    appLabel = new QLabel; appLabel->setObjectName("appLine"); appLabel->setWordWrap(true); prefixLayout->addWidget(appLabel);
    saveButton = new QPushButton("Save for this prefix"); saveButton->setObjectName("savePrefix"); prefixLayout->addWidget(saveButton, 0, Qt::AlignLeft);
    layout->addWidget(prefixGroup);
    layout->addStretch();
    auto *close = new QDialogButtonBox(QDialogButtonBox::Close); layout->addWidget(close);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(copyButton, &QPushButton::clicked, this, &DyldCacheDialog::startCopy);
    connect(verifyButton, &QPushButton::clicked, this, &DyldCacheDialog::startVerify);
    connect(refreshButton, &QPushButton::clicked, this, &DyldCacheDialog::probeSource);
    connect(cancelButton, &QPushButton::clicked, this, [this] { cancel.store(true); progressLabel->setText("Stopping…"); });
    connect(enable, &QCheckBox::toggled, this, &DyldCacheDialog::updatePrefix);
    connect(storeChoice, &QComboBox::currentIndexChanged, this, &DyldCacheDialog::updatePrefix);
    for (auto *box : libraries) connect(box, &QCheckBox::toggled, this, &DyldCacheDialog::updatePrefix);
    connect(saveButton, &QPushButton::clicked, this, &DyldCacheDialog::savePrefix);
    connect(&watcher, &QFutureWatcher<JobResult>::finished, this, [this] {
        const JobResult result = watcher.result();
        setRunning(false);
        if (onDone) onDone(result);
        emit jobFinished();
    });

    sourceLabel->setText("Reading the volume…");
    showOverview(); loadPrefix(); setRunning(false);
    probeSource();
}

DyldCacheDialog::~DyldCacheDialog() {
    cancel.store(true);
    if (running) watcher.waitForFinished();
}

void DyldCacheDialog::reject() {
    // a job keeps running in its thread; closing under it would leave it writing into a store nobody watches
    if (running) { cancel.store(true); progressLabel->setText("Stopping… close the window once the job has stopped."); return; }
    QDialog::reject();
}

void DyldCacheDialog::setRunning(bool busy) {
    running = busy;
    copyButton->setEnabled(!busy && haveSource);
    refreshButton->setEnabled(!busy);
    verifyButton->setEnabled(!busy && !overview(storeRoot, {}, {}).readyIds.isEmpty());
    cancelButton->setEnabled(busy);
    if (!busy) bar->setVisible(false);
}

void DyldCacheDialog::startJob(const std::function<JobResult()> &work, const std::function<void(const JobResult &)> &done) {
    cancel.store(false);
    onDone = done;
    setRunning(true);
    watcher.setFuture(QtConcurrent::run(work));
}

void DyldCacheDialog::probeSource() {
    sourceLabel->setText("Reading the volume…");
    const QString vol = volume;
    startJob([vol] {
        JobResult r;
        if (!resolveSourceDir(vol, &r.sourceDir, &r.sourceError)) return r;
        if (!DyldCacheStore::sourceId(r.sourceDir, kArch, &r.sourceId, &r.sourceError)) r.sourceDir.clear();
        return r;
    }, [this](const JobResult &r) {
        sourceDir = r.sourceDir; sourceId = r.sourceId; sourceError = r.sourceError; haveSource = !sourceId.isEmpty();
        sourceLabel->setText(haveSource ? "Source: " + sourceDir : "Source unavailable: " + sourceError);
        showOverview(); updatePrefix(); setRunning(false);
    });
}

void DyldCacheDialog::showOverview() {
    const StoreOverview o = overview(storeRoot, sourceId, sourceError);
    headline->setText(o.headline);
    headline->setStyleSheet(o.stale || !o.sourceKnown ? "font-weight: bold; color: #b36b00;" : QString());
    QStringList lines;
    for (const StoreRow &r : o.rows) lines << r.text;
    rows->setText(lines.join('\n'));
}

// Called from the worker thread. At most one queued update is pending, so a fast copy does not flood the event loop.
void DyldCacheDialog::postProgress(const DyldCacheCopy::Progress &p, const QString &label) {
    { QMutexLocker lock(&progressLock); latest = p; }
    if (progressPosted.exchange(true)) return;
    QMetaObject::invokeMethod(this, [this, label] {
        progressPosted.store(false);
        DyldCacheCopy::Progress p; { QMutexLocker lock(&progressLock); p = latest; }
        bar->setValue(p.totalSize > 0 ? int(p.totalBytes * 1000 / p.totalSize) : 0);
        progressLabel->setText(label + progressText(p));
    }, Qt::QueuedConnection);
}

void DyldCacheDialog::startCopy() {
    if (!haveSource) return;
    bar->setValue(0); bar->setVisible(true); progressLabel->setText("Starting…");
    const QString src = sourceDir, root = storeRoot;
    const DyldCacheCopy::Options options = copyOptions();
    startJob([this, src, root, options] {
        JobResult r;
        r.outcome = DyldCacheCopy::copyCache(src, kArch, root, options, [this](const DyldCacheCopy::Progress &p) { postProgress(p, {}); }, &cancel);
        return r;
    }, [this](const JobResult &r) {
        using R = DyldCacheCopy::Result;
        if (r.outcome.result == R::Done) progressLabel->setText("Copy finished.");
        else if (r.outcome.result == R::Cancelled) progressLabel->setText("Cancelled. The partial copy is kept; Copy resumes it.");
        else { progressLabel->setText("Copy failed: " + r.outcome.error); showError("dyld cache copy", r.outcome.error); }
        showOverview(); loadPrefix(); setRunning(false);
    });
}

void DyldCacheDialog::startVerify() {
    const QStringList ids = overview(storeRoot, sourceId, sourceError).readyIds;
    if (ids.isEmpty()) return;
    bar->setValue(0); bar->setVisible(true); progressLabel->setText("Verifying…");
    const QString root = storeRoot;
    startJob([this, ids, root] {
        JobResult r;
        r.outcome.result = DyldCacheCopy::Result::Done;
        for (const QString &id : ids) {
            QString error;
            if (!DyldCacheStore::checkPrivate(root, id, &error)) { r.lines << id + ": " + error; r.outcome.result = DyldCacheCopy::Result::Failed; continue; }
            const DyldCacheCopy::Outcome v = DyldCacheCopy::verifyStore(root, id, [this](const DyldCacheCopy::Progress &p) { postProgress(p, "Verifying: "); }, &cancel);
            if (v.result == DyldCacheCopy::Result::Cancelled) { r.outcome.result = DyldCacheCopy::Result::Cancelled; break; }
            if (v.result == DyldCacheCopy::Result::Failed) { r.lines << id + ": " + v.error; r.outcome.result = DyldCacheCopy::Result::Failed; }
        }
        r.outcome.error = r.lines.join('\n');
        return r;
    }, [this](const JobResult &r) {
        using R = DyldCacheCopy::Result;
        if (r.outcome.result == R::Done) progressLabel->setText("Verified: sizes, headers and hashes of the local store match.");
        else if (r.outcome.result == R::Cancelled) progressLabel->setText("Verification cancelled.");
        else { progressLabel->setText("Verification failed: " + r.outcome.error); showError("dyld cache verify", r.outcome.error); }
        showOverview(); loadPrefix(); setRunning(false);
    });
}

void DyldCacheDialog::loadPrefix() {
    loadFailed = false; loaded = {}; loadError.clear();
    const bool usable = !prefix.isEmpty() && QDir::isAbsolutePath(prefix) && QFileInfo(prefix).isDir();
    prefixGroup->setTitle(usable ? "This prefix" : "This prefix (none selected)");
    prefixGroup->setEnabled(usable);
    QString error;
    if (usable && !DyldCacheConfigFile::load(prefix, &loaded, &error)) { loadFailed = true; loaded = {}; loadError = error; }
    const QSignalBlocker b1(enable), b2(storeChoice);
    QStringList ready = overview(storeRoot, sourceId, sourceError).readyIds;
    storeChoice->clear();
    for (const QString &id : ready) storeChoice->addItem(id, id);
    if (!loaded.store.isEmpty() && !ready.contains(loaded.store)) storeChoice->addItem(loaded.store + " (not ready)", loaded.store);
    const int at = storeChoice->findData(!loaded.store.isEmpty() ? loaded.store : sourceId);
    if (at >= 0) storeChoice->setCurrentIndex(at);
    enable->setChecked(loaded.mode == DyldCacheMode::AllowList);
    for (auto *box : libraries) { const QSignalBlocker blocker(box); box->setChecked(loaded.served.contains(box->property("installName").toString())); }
    updatePrefix();
}

DyldCacheConfig DyldCacheDialog::editedConfig(QString *error) const {
    QStringList checked;
    for (auto *box : libraries) if (box->isChecked()) checked << box->property("installName").toString();
    DyldCacheConfig out;
    if (!buildConfig(loaded, enable->isChecked(), storeChoice->currentData().toString(), checked, &out, error)) return loaded;
    return out;
}

void DyldCacheDialog::updatePrefix() {
    const QString storeId = storeChoice->currentData().toString();
    const QString blocker = enableBlocker(storeRoot, storeId);
    // Turning it off must always be possible, even when the store it was pinned to is gone.
    enable->setEnabled(blocker.isEmpty() || enable->isChecked());
    enable->setToolTip(blocker.isEmpty() ? launchRuleHelp() : blocker);
    for (auto *box : libraries) box->setEnabled(enable->isChecked());
    QString error;
    const DyldCacheConfig edited = editedConfig(&error);
    const PrefixStatus status = prefixStatus(error.isEmpty() ? edited : loaded, storeRoot);
    prefixStatusLabel->setText(error.isEmpty() ? status.text : "Cannot save these settings: " + error);
    if (!blocker.isEmpty() && !enable->isChecked()) prefixStatusLabel->setText(prefixStatusLabel->text() + " The cache cannot be turned on yet: " + blocker + ".");
    if (loadFailed) prefixStatusLabel->setText("The prefix's cache settings cannot be read (" + loadError + "). Saving replaces them. " + prefixStatusLabel->text());
    // the pinned store only matters once the cache is on
    const bool changed = edited.mode != loaded.mode || edited.served != loaded.served || (edited.mode == DyldCacheMode::AllowList && edited.store != loaded.store);
    saveButton->setEnabled(prefixGroup->isEnabled() && error.isEmpty() && (changed || loadFailed));
    appLabel->setText(app.name.isEmpty() ? "Select an app in the launcher to see whether its launch would get the cache."
                                        : DyldCacheSettings::appLine(prefix, app.name, app.executableRel, error.isEmpty() ? edited : loaded));
}

void DyldCacheDialog::savePrefix() {
    QString error;
    const DyldCacheConfig edited = editedConfig(&error);
    if (!error.isEmpty() || !DyldCacheConfigFile::save(prefix, edited, &error)) { showError("dyld cache settings", error); return; }
    loaded = edited; loadFailed = false;
    updatePrefix();
    prefixStatusLabel->setText("Saved. " + prefixStatusLabel->text());
}
