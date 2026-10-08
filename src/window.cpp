// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"
#include "mountdialog.h"
#include "appbrowser.h"
#include "prefixdialog.h"
#include "discovery.h"
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QGridLayout>
#include <QButtonGroup>
#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMenu>
#include <QProgressBar>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QSplitter>
#include <QTableWidget>
#include <QTextEdit>
#include <QTabWidget>
#include <QTimer>
#include <QStandardItemModel>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QtConcurrent>

Window::Window(const QString &builderScript, bool mountAll, std::function<QList<SourceMount>()> provider) : mountProvider(std::move(provider)) {
    setWindowTitle("Darling Launcher — host application");
    resize(1050, 720);
    auto *central = new QWidget;
    auto *rootLayout = new QVBoxLayout(central);
    auto *tabs = new QTabWidget; tabs->setObjectName("mainTabs"); rootLayout->addWidget(tabs);
    auto *appsPage = new QWidget; appsPage->setObjectName("appsPage");
    auto *settingsPage = new QWidget; settingsPage->setObjectName("settingsPage");
    tabs->addTab(appsPage, "Apps"); tabs->addTab(settingsPage, "Settings");
    auto *layout = new QVBoxLayout(appsPage);
    auto *settingsLayout = new QVBoxLayout(settingsPage);
    settingsLayout->addWidget(new QLabel("Choose your macOS source and isolated Darling runtime. Paths are detected where possible."));
    auto *form = new QFormLayout;
    auto locationRow = [&](const QString &label, QLineEdit *&field, bool file) {
        auto *row = new QWidget;
        auto *box = new QHBoxLayout(row); box->setContentsMargins(0,0,0,0);
        field = new QLineEdit; box->addWidget(field);
        auto *browse = new QPushButton("Browse…"); box->addWidget(browse);
        connect(browse, &QPushButton::clicked, this, [this, field, file] {
            QString path = file ? QFileDialog::getOpenFileName(this, "Select Darling executable")
                                : QFileDialog::getExistingDirectory(this, "Select directory");
            if (!path.isEmpty()) field->setText(path);
        });
        form->addRow(label, row);
    };
    locationRow("Mounted macOS volume", volume, false);
    locationRow("Darling prefix", prefix, false);
    locationRow("Darling executable", darling, true);
    locationRow("Runtime install root (optional)", runtimeRoot, false);
    runtimeRoot->setObjectName("runtimeRootField");
    volume->setObjectName("volumeField"); prefix->setObjectName("prefixField"); darling->setObjectName("darlingField");
    sourceChoices = new QComboBox; sourceChoices->setObjectName("mountedSourceChoices"); form->addRow("Existing mounted volumes", sourceChoices);
    sourceSummary = new QLabel; sourceSummary->setObjectName("mountedSourceSummary"); sourceSummary->setWordWrap(true); settingsLayout->addWidget(sourceSummary);
    connect(sourceChoices, &QComboBox::activated, this, [this](int index) {
        const QString root = sourceChoices->itemData(index).toString();
        for (const auto &candidate : LauncherSources::candidates(mountProvider())) if (candidate.root == root && candidate.usable()) {
            volume->setText(root); refresh(); QSettings("cristim", "darling-launcher").setValue("volume", root); return;
        }
        updateSourceChoices();
    });
    settingsLayout->addLayout(form);
    QSettings settings("cristim", "darling-launcher");
    volume->setText(settings.value("volume").toString());
    prefix->setText(settings.value("prefix").toString());
    darling->setText(settings.value("darling").toString());
    runtimeRoot->setText(settings.value("runtimeRoot").toString());
    const QString dataRoot = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (prefix->text().isEmpty()) prefix->setText(LauncherDiscovery::managedPrefix(dataRoot));
    volume->setPlaceholderText("Select a mounted source with apps or libraries");
    auto *runtimeChoices = new QComboBox; runtimeChoices->setObjectName("detectedRuntimes");
    runtimeChoices->addItem("Select a detected runtime…");
    form->addRow("Detected runtimes", runtimeChoices);
    connect(runtimeChoices, &QComboBox::activated, this, [this, runtimeChoices](int index) {
        if (index <= 0) return;
        darling->setText(runtimeChoices->itemData(index).toString()); runtimeRoot->setText(runtimeChoices->itemData(index, Qt::UserRole + 1).toString());
    });
    auto detectPaths = [this, runtimeChoices] {
        const auto detectedRuntimes = LauncherDiscovery::runtimes(LauncherDiscovery::roots(), QStandardPaths::findExecutable("darling"));
        QSignalBlocker blocker(runtimeChoices); runtimeChoices->clear();
        runtimeChoices->addItem(detectedRuntimes.isEmpty() ? "No usable runtime found — build or select one" : "Choose a detected runtime…");
        for (const auto &runtime : detectedRuntimes) {
            runtimeChoices->addItem(runtime.launcher + " — " + runtime.installRoot, runtime.launcher);
            runtimeChoices->setItemData(runtimeChoices->count() - 1, runtime.installRoot, Qt::UserRole + 1);
        }
        volume->setText(LauncherSources::automaticSource(LauncherSources::candidates(mountProvider()), volume->text()));
        if ((darling->text().isEmpty() || !QFileInfo(darling->text()).isExecutable()) && !detectedRuntimes.isEmpty()) {
            darling->setText(detectedRuntimes.first().launcher); runtimeRoot->setText(detectedRuntimes.first().installRoot);
        }
        if (runtimeRoot->text().isEmpty()) for (const auto &runtime : detectedRuntimes)
            if (runtime.launcher == darling->text()) { runtimeRoot->setText(runtime.installRoot); break; }
        QFile provenance(prefix->text() + "/.darling-launcher/build-provenance.json");
        if (!prefix->text().isEmpty() && provenance.open(QIODevice::ReadOnly)) {
            auto data = QJsonDocument::fromJson(provenance.readAll()).object();
            QString launcher = data.value("launcher").toString();
            QString runtime = data.value("runtime_install_root").toString();
            if (QFileInfo(launcher).isExecutable()) darling->setText(launcher);
            if (QFileInfo(runtime).isDir()) runtimeRoot->setText(runtime);
        }
        for (int index = 1; index < runtimeChoices->count(); ++index)
            if (runtimeChoices->itemData(index).toString() == darling->text() && runtimeChoices->itemData(index, Qt::UserRole + 1).toString() == runtimeRoot->text()) { runtimeChoices->setCurrentIndex(index); break; }
    };
    detectPaths();
    connect(prefix, &QLineEdit::editingFinished, this, detectPaths);
    for (QLineEdit *field : {volume, prefix, darling, runtimeRoot})
        connect(field, &QLineEdit::editingFinished, this, [this] {
            QSettings settings("cristim", "darling-launcher");
            settings.setValue("volume", volume->text()); settings.setValue("prefix", prefix->text()); settings.setValue("darling", darling->text());
            settings.setValue("runtimeRoot", runtimeRoot->text());
        });
    auto *toolbar = new QGridLayout;
    auto *settingsToolbar = new QGridLayout;
    int actionCount = 0, settingsActionCount = 0;
    auto add = [&](const QString &title, auto callback) {
        auto *button = new QPushButton(title == "Create prefix" ? "Build and deploy Darling…" : title); button->setObjectName(title);
        bool setting = title == "Detect paths" || title == "Create prefix" || title == "Initialize selected prefix" || title == "Detect mounted macOS volumes" || title == "Mount macOS source…" || title == "Apply settings";
        int &count = setting ? settingsActionCount : actionCount;
        (setting ? settingsToolbar : toolbar)->addWidget(button, count / 3, count % 3); ++count;
        connect(button, &QPushButton::clicked, this, callback);
        return button;
    };
    add("Detect paths", [this, detectPaths] { detectPaths(); updateSourceChoices(); refresh(); load(); });
    add("Apply settings", [this, tabs] {
        QSettings settings("cristim", "darling-launcher");
        settings.setValue("volume", volume->text()); settings.setValue("prefix", prefix->text());
        settings.setValue("darling", darling->text()); settings.setValue("runtimeRoot", runtimeRoot->text());
        refresh(); load(); tabs->setCurrentIndex(0);
    });
    add("Create prefix", [this, builderScript] {
        if (!prefixDialog) {
            prefixDialog = new PrefixDialog(volume->text(), builderScript, this);
            connect(prefixDialog, &PrefixDialog::prefixReady, this, [this](const QString &path, const QString &launcher, const QString &runtime) {
                prefix->setText(path); darling->setText(launcher); runtimeRoot->setText(runtime); load();
                QSettings settings("cristim", "darling-launcher"); settings.setValue("prefix", path); settings.setValue("darling", launcher); settings.setValue("runtimeRoot", runtime);
                setBusy(activeProcesses > 0 || importRunning, "New isolated prefix ready: " + path);
            });
        }
        prefixDialog->show(); prefixDialog->raise(); prefixDialog->activateWindow();
    });
    add("Initialize selected prefix", [this] {
        QString path = prefix->text();
        QString mounted = QFileInfo(volume->text()).canonicalFilePath();
        if (!mounted.isEmpty() && QDir::cleanPath(path).startsWith(mounted + '/')) {
            QMessageBox::warning(this, "Prefix", "The prefix cannot be inside the selected source volume."); return;
        }
        if (!QDir::isAbsolutePath(path) || path == "/" || QFileInfo(path).isSymLink() || !QDir().mkpath(path)) {
            QMessageBox::warning(this, "Prefix", "Choose an absolute, writable prefix path."); return;
        }
        runCommand("Initialize prefix", {"shell", "/usr/bin/true"});
    });
    add("Scan volume", [this] {
        if (!QFileInfo(volume->text()).isDir()) { setBusy(false, "No mounted macOS source selected."); return; }
        refresh(); load();
    });
    add("Detect mounted macOS volumes", [this, tabs] {
        updateSourceChoices(); refresh(); tabs->setCurrentIndex(1);
        setBusy(false, sourceSummary->text());
    });
    add("Mount macOS source…", [this] {
        if (!mountDialog) {
            mountDialog = new MountDialog(this, mountProvider);
            connect(mountDialog, &MountDialog::sourceMounted, this, [this](const QString &path) {
                volume->setText(path); updateSourceChoices(); refresh(); setBusy(false, "Selected mounted macOS source: " + path);
                QSettings("cristim", "darling-launcher").setValue("volume", path);
            });
        }
        mountDialog->show(); mountDialog->raise(); mountDialog->activateWindow();
    });
    add("Import selected apps", [this] { importBundles(available->selectedBundles()); });
    add("Launch selected", [this] { QString key = selectedKey(); if (!key.isEmpty()) launch(key); });
    add("Stop selected prefix processes", [this] { runCommand("Stop prefix", {"shutdown"}); });
    libraryRetry = add("Import needed library and retry", [this] {
        QString key = selectedKey();
        if (!pending.contains(key)) { QMessageBox::information(this, "Diagnosis", "Select an app with a missing loader dependency."); return; }
        if (runningApps.contains(key)) { QMessageBox::information(this, "Diagnosis", "The launch process is still running after the loader error. Choose Stop selected prefix processes, then retry. This stops all apps in this prefix."); return; }
        if (importRunning) { statusBar()->showMessage("An import is already in progress."); return; }
        const auto missing = pending.value(key);
        const QString source = volume->text(), destination = prefix->text();
        QJsonObject step{{"symbol", missing.symbol}, {"failure", missing.missingLibrary ? "missing library" : "missing symbol"}, {"library", missing.expectedIn}, {"referencedFrom", missing.referencedFrom}, {"action", "imported from selected volume"}, {"loaderOutput", outputs.value(key)}};
        step.insert("sourceVolume", source); step.insert("sourceLibrary", source + missing.expectedIn); step.insert("destinationPrefix", destination);
        auto *watcher = new QFutureWatcher<QString>(this);
        importRunning = true; setBusy(true, "Importing library: " + missing.expectedIn);
        connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, key, step, destination] {
            QString error = watcher->result(); watcher->deleteLater(); importRunning = false;
            if (!error.isEmpty()) {
                setBusy(activeProcesses > 0, "Library import failed: " + error);
                QMessageBox::warning(this, "Library import", error); return;
            }
            auto catalog = LauncherCore::loadCatalog(destination); auto records = catalog.value("apps").toArray();
            QJsonArray recordedChain; bool found = false;
            for (int index = 0; index < records.size(); ++index) {
                auto record = records[index].toObject();
                if (record.value("bundle").toString() != key) continue;
                recordedChain = record.value("chain").toArray(); recordedChain.append(step);
                record.insert("chain", recordedChain); records[index] = record; found = true; break;
            }
            catalog.insert("apps", records);
            if (!found || !LauncherCore::saveCatalog(destination, catalog, &error)) {
                setBusy(activeProcesses > 0, "Library copied, but dependency recording failed: " + error); return;
            }
            if (prefix->text() != destination) {
                setBusy(activeProcesses > 0, "Library imported and recorded in " + destination + ". Select that prefix to retry."); return;
            }
            chains[key] = recordedChain; pending.remove(key); updateContribution(); launch(key);
        });
        watcher->setFuture(QtConcurrent::run([source, destination, missing] {
            QString error; LauncherCore::importLibrary(source, destination, missing.expectedIn, &error); return error;
        }));
    });
    add("Install Brewfile", [this] {
        QString source = QFileDialog::getOpenFileName(this, "Select Brewfile"); if (source.isEmpty()) return;
        QString p = prefix->text();
        QString brew = p + "/opt/homebrew/bin/brew";
        if (!QFileInfo(brew).isFile() || QFileInfo(brew).isSymLink()) { QMessageBox::warning(this, "Brewfile", "Native guest Homebrew is not installed in this prefix."); return; }
        QString guestPath, error;
        if (!LauncherCore::stageBrewfile(p, source, &guestPath, &error)) { QMessageBox::warning(this, "Brewfile", error); return; }
        runCommand("Brewfile", {"exec", "/opt/homebrew/bin/brew", "bundle", "--file", guestPath});
    });
    settingsLayout->addLayout(settingsToolbar); settingsLayout->addStretch();
    layout->addLayout(toolbar);
    auto *split = new QSplitter(Qt::Horizontal);
    auto *sourcePane = new QWidget; auto *sourceLayout = new QVBoxLayout(sourcePane);
    auto *views = new QHBoxLayout; views->addWidget(new QLabel("Available apps")); views->addStretch();
    auto *group = new QButtonGroup(this);
    auto *list = new QPushButton("List"); list->setObjectName("listView"); list->setCheckable(true); list->setChecked(true);
    auto *grid = new QPushButton("Grid"); grid->setObjectName("gridView"); grid->setCheckable(true);
    group->addButton(list); group->addButton(grid); views->addWidget(list); views->addWidget(grid); sourceLayout->addLayout(views);
    available = new AppBrowser; available->setObjectName("availableApps"); sourceLayout->addWidget(available);
    sourceLayout->addWidget(new QLabel("Ctrl+A selects all · Ctrl-click toggles · Shift-click selects a range"));
    connect(list, &QPushButton::clicked, this, [this] { available->setGridView(false); });
    connect(grid, &QPushButton::clicked, this, [this] { available->setGridView(true); });
    auto *importPane = new QWidget; auto *importLayout = new QVBoxLayout(importPane);
    importLayout->addWidget(new QLabel("Imported apps — drag selected apps here to import"));
    apps = new ImportTable; apps->setObjectName("importedApps"); apps->setHorizontalHeaderLabels({"Imported app", "Bundle", "Status"});
    connect(apps, &ImportTable::bundlesDropped, this, &Window::importBundles);
    apps->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); apps->setSelectionBehavior(QAbstractItemView::SelectRows);
    importLayout->addWidget(apps);
    split->addWidget(sourcePane); split->addWidget(importPane); split->setStretchFactor(1, 2);
    layout->addWidget(split);
    contributionPanel = new QWidget; contributionPanel->setObjectName("contributionPanel");
    auto *contributionLayout = new QVBoxLayout(contributionPanel);
    contributionMessage = new QLabel; contributionMessage->setObjectName("contributionMessage"); contributionMessage->setWordWrap(true); contributionMessage->setTextFormat(Qt::PlainText);
    contributionLayout->addWidget(contributionMessage);
    auto *bottom = new QHBoxLayout;
    auto *draft = new QPushButton("Save proposed issue"); draft->setObjectName("saveProposedIssue"); bottom->addWidget(draft);
    connect(draft, &QPushButton::clicked, this, [this] {
        QString key = selectedKey(); if (!entries.contains(key) || chains.value(key).isEmpty()) return;
        QString path = prefix->text() + "/.darling-launcher/proposed-vibedarling-issue.md";
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) { QMessageBox::warning(this, "Draft", file.errorString()); return; }
        file.write(LauncherCore::issueDraft(entries.value(key), chains.value(key), outputs.value(key), volume->text(), prefix->text(), darling->text(), runtimeRoot->text()).toUtf8());
        if (!file.commit()) { QMessageBox::warning(this, "Draft", file.errorString()); return; }
        QMessageBox::information(this, "Draft saved", path + "\nReview and approve before any submission.");
    });
    auto *fix = new QPushButton("Opt in: source fix workflow"); fix->setObjectName("sourceFixWorkflow"); bottom->addWidget(fix);
    connect(fix, &QPushButton::clicked, this, [this] {
        if (chains.value(selectedKey()).isEmpty()) return;
        QMessageBox::information(this, "Separate source fix workflow", "After reviewing the issue draft, opt in separately to a clean-room VibeDarling source change. Work from published source, headers, documentation and API observations only. Prepare a patch, tests and PR draft for your review; no PR is submitted automatically.");
    });
    contributionLayout->addLayout(bottom); layout->addWidget(contributionPanel);
    contributionPanel->hide(); libraryRetry->hide();
    connect(apps, &QTableWidget::itemSelectionChanged, this, &Window::updateContribution);
    progress = new QProgressBar; progress->setRange(0, 1); progress->setValue(0); layout->addWidget(progress);
    log = new QTextEdit; log->setReadOnly(true); layout->addWidget(log);
    setCentralWidget(central);
    updateSourceChoices(); load();
    statusBar()->showMessage(sourceSummary->text());
    connect(prefix, &QLineEdit::textChanged, this, [this] { pending.clear(); updateContribution(); });
    auto *mountRefresh = new QTimer(this); mountRefresh->setInterval(3000);
    connect(mountRefresh, &QTimer::timeout, this, &Window::updateSourceChoices); mountRefresh->start();
    if (mountAll) QTimer::singleShot(0, this, [this, tabs] {
        tabs->setCurrentIndex(1); findChild<QPushButton *>("Mount macOS source…")->click();
        mountDialog->mountAllWhenReady();
    });
}
Window::~Window() {
    for (auto *process : findChildren<QProcess *>(QString(), Qt::FindDirectChildrenOnly)) process->disconnect(this);
}

void Window::updateSourceChoices() {
    const auto items = LauncherSources::candidates(mountProvider());
    const QString previous = volume->text();
    const QString chosen = LauncherSources::automaticSource(items, previous);
    QSignalBlocker blocker(sourceChoices); sourceChoices->clear(); sourceChoices->addItem("Choose a usable mounted source…", QString());
    int usable = 0; QStringList signature, usableRoots;
    for (const auto &item : items) {
        sourceChoices->addItem(item.description(), item.root);
        signature << item.description();
        if (item.usable()) { ++usable; usableRoots << item.root; }
        else if (auto *model = qobject_cast<QStandardItemModel *>(sourceChoices->model())) model->item(sourceChoices->count() - 1)->setEnabled(false);
    }
    if (usable == 0) sourceChoices->setItemText(0, items.isEmpty() ? "No mounted macOS volumes" : "No usable sources — mounted volumes have no app/library directories");
    int index = usableRoots.contains(chosen) ? sourceChoices->findData(chosen) : 0; sourceChoices->setCurrentIndex(index < 0 ? 0 : index);
    sourceSummary->setText(items.isEmpty() ? "No mounted macOS volumes detected." : QString::number(items.size()) + " mounted volumes detected; " + QString::number(usable) + " suitable for app or library import. " + (usable > 1 ? "Choose a source; multiple usable volumes are mounted." : usable == 0 ? "These mounts contain no readable app/library directories." : "The usable source is selected automatically."));
    if (chosen != previous) { volume->setText(chosen); QSettings("cristim", "darling-launcher").setValue("volume", chosen); }
    QString updated = signature.join('\n');
    if (!sourceChoicesInitialized || chosen != previous || updated != sourceSignature) refresh();
    sourceChoicesInitialized = true; sourceSignature = updated;
}

void Window::updateContribution() {
    const QString key = selectedKey();
    libraryRetry->setVisible(pending.contains(key));
    const auto chain = chains.value(key);
    contributionPanel->setVisible(entries.contains(key) && !chain.isEmpty());
    if (chain.isEmpty()) return;
    QStringList libraries;
    for (const auto &step : chain) libraries << step.toObject().value("library").toString();
    QString result;
    const int row = apps->currentRow();
    if (row >= 0 && apps->item(row, 2)) result = apps->item(row, 2)->text();
    contributionMessage->setText(entries.value(key).name + " failed to launch because a loader dependency was missing. "
        "At your request, we copied these libraries from your macOS volume into this private prefix and retried: "
        + libraries.join(", ") + ".\nCurrent result: " + result + ".\n"
        "This is a local workaround using Apple libraries. You can help VibeDarling implement the missing functionality "
        "by reviewing a proposed issue with the reproduction steps, loader error and dependency chain. "
        "A source fix is a separate opt-in contribution using published sources, headers and documented behavior. "
        "Libraries remain private; nothing is submitted until you approve a completed draft.");
}

void Window::setBusy(bool busy, const QString &message) { busy = busy || importRunning; progress->setRange(0, busy ? 0 : 1); if (!busy) progress->setValue(1); statusBar()->showMessage(message); log->append(message); }
void Window::refresh() {
    const QString source = volume->text(); const int generation = ++scanGeneration;
    available->clear();
    if (source.isEmpty() || !QFileInfo(source).isDir()) return;
    auto *watcher = new QFutureWatcher<QList<AppPreview>>(this);
    setBusy(true, "Reading application names, icons and sizes…");
    connect(watcher, &QFutureWatcher<QList<AppPreview>>::finished, this, [this, watcher, generation, source] {
        auto previews = watcher->result(); watcher->deleteLater();
        if (generation != scanGeneration || volume->text() != source) return;
        available->showPreviews(previews); setBusy(activeProcesses > 0 || importRunning, QString::number(previews.size()) + " apps available");
    });
    watcher->setFuture(QtConcurrent::run([source] {
        QList<AppPreview> previews;
        for (const QString &name : LauncherCore::discoverApps(source)) {
            auto preview = LauncherCore::appPreview(source, name);
            if (!preview.relativeBundle.isEmpty()) previews << preview;
        }
        return previews;
    }));
}
void Window::importBundles(const QStringList &names) {
    if (names.isEmpty()) return;
    if (importRunning) { statusBar()->showMessage("An app import is already in progress."); return; }
    QString error;
    if (!LauncherCore::validateLocations(volume->text(), prefix->text(), &error)) { QMessageBox::warning(this, "Locations", error); return; }
    auto *watcher = new QFutureWatcher<QPair<QList<AppEntry>, QString>>(this);
    importRunning = true; setBusy(true, "Importing selected apps…");
    const QString v = volume->text(), p = prefix->text();
    connect(watcher, &QFutureWatcher<QPair<QList<AppEntry>, QString>>::finished, this, [this, watcher, p] {
        auto result = watcher->result(); watcher->deleteLater(); importRunning = false;
        auto catalog = LauncherCore::loadCatalog(p); auto array = catalog.value("apps").toArray();
        for (const auto &entry : result.first)
            array.append(QJsonObject{{"name", entry.name}, {"bundle", entry.relativeBundle}, {"executable", entry.executable}, {"source", entry.sourceRelative}, {"chain", QJsonArray{}}});
        catalog.insert("apps", array); QString error;
        if (!LauncherCore::saveCatalog(p, catalog, &error)) result.second += "\n" + error;
        if (prefix->text() == p) load();
        setBusy(activeProcesses > 0, result.second.isEmpty() ? "Import complete" : result.second);
        if (!result.second.isEmpty()) QMessageBox::warning(this, "Import", result.second);
    });
    watcher->setFuture(QtConcurrent::run([v, p, names] {
        QList<AppEntry> result; QString error;
        for (const QString &name : names) {
            AppEntry entry;
            if (!LauncherCore::importApp(v, p, name, &entry, &error)) break;
            result << entry;
        }
        return qMakePair(result, error);
    }));
}
void Window::load() {
    entries.clear(); chains.clear();
    auto catalog = LauncherCore::loadCatalog(prefix->text());
    for (const auto &item : catalog.value("apps").toArray()) {
        auto obj = item.toObject(); AppEntry entry{obj.value("name").toString(), obj.value("bundle").toString(), obj.value("executable").toString(), obj.value("source").toString()};
        if (!entry.relativeBundle.isEmpty()) { entries.insert(entry.relativeBundle, entry); chains.insert(entry.relativeBundle, obj.value("chain").toArray()); }
    }
    apps->setRowCount(entries.size()); int row = 0;
    for (auto it = entries.cbegin(); it != entries.cend(); ++it, ++row) {
        apps->setItem(row, 0, new QTableWidgetItem(it.value().name));
        apps->setItem(row, 1, new QTableWidgetItem(it.key()));
        apps->setItem(row, 2, new QTableWidgetItem("Ready"));
    }
    updateContribution();
}
void Window::persist() {
    QJsonArray array;
    for (auto it = entries.cbegin(); it != entries.cend(); ++it)
        array.append(QJsonObject{{"name", it.value().name}, {"bundle", it.key()}, {"executable", it.value().executable}, {"source", it.value().sourceRelative}, {"chain", chains.value(it.key())}});
    QString error;
    if (!LauncherCore::saveCatalog(prefix->text(), QJsonObject{{"apps", array}}, &error)) QMessageBox::warning(this, "Catalog", error);
}
QString Window::selectedKey() const { int row = apps->currentRow(); return row < 0 || !apps->item(row, 1) ? QString() : apps->item(row, 1)->text(); }
void Window::launch(const QString &key) {
    if (!entries.contains(key)) return;
    if (runningApps.contains(key)) { setBusy(true, "This app already has a running launch process."); return; }
    pending.remove(key);
    const AppEntry app = entries.value(key);
    outputs[key].clear();
    for (int row = 0; row < apps->rowCount(); ++row) if (apps->item(row, 1)->text() == key) apps->item(row, 2)->setText("Launching…");
    updateContribution();
    runCommand(app.name, {"exec", "/" + app.relativeBundle + "/Contents/MacOS/" + app.executable}, key);
}
void Window::runCommand(const QString &label, const QStringList &args, const QString &key) {
    if (!QFileInfo(darling->text()).isExecutable()) { QMessageBox::warning(this, label, "Select an executable Darling host launcher."); return; }
    QString p = prefix->text();
    if (!QDir::isAbsolutePath(p) || !QFileInfo(p).isDir()) { QMessageBox::warning(this, label, "Select an existing prefix directory."); return; }
    if (!runtimeRoot->text().isEmpty() && (!QDir::isAbsolutePath(runtimeRoot->text()) || !QFileInfo(runtimeRoot->text()).isDir())) { QMessageBox::warning(this, label, "Select a valid runtime install root."); return; }
    auto *process = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const auto &name : env.keys()) if (name.startsWith("DYLD_")) env.remove(name);
    env.remove("DARLING_INSTALL_PREFIX");
    if (!runtimeRoot->text().isEmpty()) env.insert("DARLING_INSTALL_PREFIX", runtimeRoot->text());
    env.insert("DPREFIX", p);
    process->setProcessEnvironment(env); process->setProcessChannelMode(QProcess::MergedChannels);
    ++activeProcesses;
    if (!key.isEmpty()) runningApps.insert(key);
    setBusy(true, "Running " + label);
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process, key, p] {
        QString text = QString::fromLocal8Bit(process->readAllStandardOutput()); log->insertPlainText(text); log->ensureCursorVisible();
        if (!key.isEmpty() && prefix->text() == p) { outputs[key] += text; diagnoseOutput(key); }
    });
    connect(process, &QProcess::finished, this, [this, process, key, label, p](int code, QProcess::ExitStatus) {
        --activeProcesses;
        runningApps.remove(key);
        setBusy(activeProcesses > 0, label + " exited with status " + QString::number(code));
        if (!key.isEmpty() && prefix->text() == p) {
            outputs[key] += QString::fromLocal8Bit(process->readAllStandardOutput());
            MissingSymbol missing = LauncherCore::diagnose(outputs.value(key));
            int row = -1; for (int i = 0; i < apps->rowCount(); ++i) if (apps->item(i, 1)->text() == key) row = i;
            if (missing.valid() && code != 0) {
                diagnoseOutput(key);
            } else if (row >= 0) {
                apps->item(row, 2)->setText(code == 0 ? "Exited successfully" : "Exited " + QString::number(code));
                if (code == 0 && !chains.value(key).isEmpty())
                    setBusy(activeProcesses > 0, label + " succeeded after importing macOS libraries. Review the contribution offer below to help VibeDarling.");
            }
        }
        updateContribution();
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process, label, key](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) { --activeProcesses; runningApps.remove(key); setBusy(activeProcesses > 0, label + ": " + process->errorString()); process->deleteLater(); }
    });
    process->start(darling->text(), args);
}

void Window::diagnoseOutput(const QString &key) {
    if (pending.contains(key)) return;
    const auto missing = LauncherCore::diagnose(outputs.value(key));
    if (!missing.valid()) return;
    pending.insert(key, missing);
    const QString description = missing.missingLibrary ? "Library not loaded: " + missing.expectedIn : "Missing " + missing.symbol + " in " + missing.expectedIn;
    for (int row = 0; row < apps->rowCount(); ++row) if (apps->item(row, 1)->text() == key) apps->item(row, 2)->setText(description);
    setBusy(activeProcesses > 0, description + ". Import needed library and retry; stop the selected prefix first if its launch process is still running.");
    updateContribution();
}

void Window::closeEvent(QCloseEvent *event) {
    if (mountDialog && mountDialog->isMounting()) {
        statusBar()->showMessage("Complete or dismiss the authentication prompt before closing the launcher.");
        event->ignore(); return;
    }
    QMainWindow::closeEvent(event);
}
