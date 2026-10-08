// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"
#include "mountdialog.h"
#include "appbrowser.h"
#include "prefixdialog.h"
#include "discovery.h"
#include "troubleshooting.h"
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
#include <QPromise>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QSplitter>
#include <QTextEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QTimer>
#include <QStandardItemModel>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QtConcurrent>

Window::Window(const QString &builderScript, bool mountAll, std::function<QList<SourceMount>()> provider) : mountProvider(std::move(provider)) {
    setWindowTitle("Darling Launcher — host application");
    resize(1050, 720);
    restoreGeometry(QSettings("cristim", "darling-launcher").value("windowGeometry").toByteArray());
    auto *central = new QWidget;
    auto *rootLayout = new QVBoxLayout(central);
    auto *settingsButton = new QPushButton("Settings…"); settingsButton->setObjectName("openSettings");
    prefixChoices = new QComboBox; prefixChoices->setObjectName("prefixChoices"); prefixChoices->setMinimumContentsLength(24); prefixChoices->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    auto *top = new QHBoxLayout; top->addWidget(new QLabel("Import and launch in")); top->addWidget(prefixChoices); top->addStretch(); top->addWidget(settingsButton); rootLayout->addLayout(top);
    settingsDialog = new QDialog(this); settingsDialog->setObjectName("settingsDialog"); settingsDialog->setWindowTitle("Launcher settings"); settingsDialog->resize(860, 520);
    connect(settingsButton, &QPushButton::clicked, this, [this] { settingsDialog->show(); settingsDialog->raise(); settingsDialog->activateWindow(); });
    auto *appsPage = new QWidget; appsPage->setObjectName("appsPage");
    auto *settingsPage = settingsDialog;
    rootLayout->addWidget(appsPage);
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
    updatePrefixChoices();
    connect(prefixChoices, &QComboBox::activated, this, [this, detectPaths](int index) {
        const QString path = prefixChoices->itemData(index).toString();
        if (path.isEmpty() || path == prefix->text()) return;
        if (activeProcesses > 0 || importRunning) { statusBar()->showMessage("Finish imports and stop this prefix’s processes before switching prefixes."); updatePrefixChoices(); return; }
        prefix->setText(path); pending.clear(); failedApps.clear(); outputs.clear(); recoveryOutcomes.clear(); launchChoices.clear(); recoveryActions.clear(); failurePreferencesShown.clear(); recoveryReady.clear(); detectPaths(); load();
        QSettings settings("cristim", "darling-launcher"); settings.setValue("prefix", path); settings.setValue("darling", darling->text()); settings.setValue("runtimeRoot", runtimeRoot->text());
        updatePrefixChoices();
    });
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
        bool setting = title == "Detect paths" || title == "Create prefix" || title == "Initialize selected prefix" || title == "Detect mounted macOS volumes" || title == "Mount macOS source…" || title == "Apply settings" || title == "Stop background agents" || title == "Launch recovery preferences…";
        int &count = setting ? settingsActionCount : actionCount;
        (setting ? settingsToolbar : toolbar)->addWidget(button, count / 3, count % 3); ++count;
        connect(button, &QPushButton::clicked, this, callback);
        return button;
    };
    add("Launch recovery preferences…", [this] {
        LaunchChoicesDialog choices("future apps", hasMountedSource(), settingsDialog);
        choices.setSourceAvailability(hasMountedSource(), hasPossibleMacOSSource());
        connect(&choices, &LaunchChoicesDialog::mountRequested, this, [this] { findChild<QPushButton *>("Mount macOS source…")->click(); });
        connect(volume, &QLineEdit::textChanged, &choices, [this, &choices] { choices.setSourceAvailability(hasMountedSource(), hasPossibleMacOSSource()); });
        if (choices.exec() == QDialog::Accepted) LauncherTroubleshooting::saveRecoveryChoices(choices.choices());
    });
    connect(volume, &QLineEdit::textChanged, this, [this] { for (const auto &key : recoveryDialogs.keys()) updateRecovery(key); });
    add("Stop background agents", [this] { for (auto *job : findChildren<BackgroundFix *>()) if (job->isRunning()) job->stop(); });
    add("Detect paths", [this, detectPaths] { detectPaths(); updateSourceChoices(); refresh(); load(); });
    add("Apply settings", [this] {
        QSettings settings("cristim", "darling-launcher");
        settings.setValue("volume", volume->text()); settings.setValue("prefix", prefix->text());
        settings.setValue("darling", darling->text()); settings.setValue("runtimeRoot", runtimeRoot->text());
        refresh(); load(); settingsDialog->hide();
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
    add("Detect mounted macOS volumes", [this] {
        discoverMacPartitions(); updateSourceChoices(); refresh(); settingsDialog->show();
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
    add("Launch selected", [this] { for (const auto &key : apps->selectedBundles()) launch(key); });
    add("Stop selected prefix processes", [this] { runCommand("Stop prefix", {"shutdown"}); });
    troubleshoot = add("Troubleshoot failed app…", [this] { openTroubleshooting(); }); troubleshoot->hide();
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
    auto *closeSettings = new QDialogButtonBox(QDialogButtonBox::Close); settingsLayout->addWidget(closeSettings);
    connect(closeSettings, &QDialogButtonBox::rejected, settingsDialog, &QDialog::hide);
    layout->addLayout(toolbar);
    auto *split = new QSplitter(Qt::Horizontal); browserSplitter = split; split->setObjectName("browserSplitter");
    auto *sourcePane = new QWidget; auto *sourceLayout = new QVBoxLayout(sourcePane);
    sourceLayout->addWidget(new QLabel("Available apps"));
    auto *views = new QHBoxLayout; views->addStretch();
    auto *group = new QButtonGroup(this);
    auto *list = new QPushButton("List"); list->setObjectName("listView"); list->setCheckable(true); list->setChecked(true);
    auto *grid = new QPushButton("Grid"); grid->setObjectName("gridView"); grid->setCheckable(true);
    group->addButton(list); group->addButton(grid); views->addWidget(list); views->addWidget(grid);
    auto *sort = new QComboBox; sort->setObjectName("appSort"); sort->addItems({"Name A–Z", "Name Z–A", "Size smallest first", "Size largest first"}); views->addWidget(sort); sourceLayout->addLayout(views);
    auto *sourceSearch = new QLineEdit; sourceSearch->setObjectName("sourceSearch"); sourceSearch->setPlaceholderText("Search available apps"); sourceSearch->setClearButtonEnabled(true); sourceLayout->addWidget(sourceSearch);
    auto *sourceFilter = new QComboBox; sourceFilter->setObjectName("sourceFilter"); sourceFilter->addItems({"All apps", "Already imported"}); sourceLayout->addWidget(sourceFilter);
    sourceEmpty = new QLabel("Select a mounted macOS source in Settings, then choose apps to import."); sourceEmpty->setObjectName("sourceEmptyState"); sourceEmpty->setWordWrap(true); sourceLayout->addWidget(sourceEmpty);
    auto *chooseSource = new QPushButton("Choose macOS source…"); chooseSource->setObjectName("emptyChooseSource"); sourceLayout->addWidget(chooseSource);
    connect(chooseSource, &QPushButton::clicked, settingsButton, &QPushButton::click);
    available = new AppBrowser; available->setObjectName("availableApps"); sourceLayout->addWidget(available);
    connect(available, &AppBrowser::visibleAppsChanged, this, [this, chooseSource](int count) { sourceEmpty->setVisible(count == 0); chooseSource->setVisible(count == 0 && volume->text().isEmpty()); sourceEmpty->setText(count > 0 ? QString() : available->count() > 0 ? "No apps match this search or filter." : volume->text().isEmpty() ? "No macOS source selected. Choose an existing readable mount in Settings, or explicitly mount a volume." : "No apps found on this source. Check the selected volume in Settings."); });
    connect(sourceSearch, &QLineEdit::textChanged, available, &AppBrowser::setSearch);
    connect(sourceFilter, &QComboBox::currentIndexChanged, this, [this](int index) { available->setFilter(index == 1 ? AppBrowser::Filter::Imported : AppBrowser::Filter::All); });
    auto *selectionHint = new QLabel("Ctrl+A selects all · Ctrl-click toggles · Shift-click selects a range"); selectionHint->setWordWrap(true); sourceLayout->addWidget(selectionHint);
    connect(list, &QPushButton::clicked, this, [this] { available->setGridView(false); apps->setGridView(false); QSettings("cristim", "darling-launcher").setValue("gridView", false); });
    connect(grid, &QPushButton::clicked, this, [this] { available->setGridView(true); apps->setGridView(true); QSettings("cristim", "darling-launcher").setValue("gridView", true); });
    auto *importPane = new QWidget; auto *importLayout = new QVBoxLayout(importPane);
    auto *importTitle = new QLabel("Imported apps — drag selected apps here to import"); importTitle->setWordWrap(true); importLayout->addWidget(importTitle);
    auto *appSearch = new QLineEdit; appSearch->setObjectName("appSearch"); appSearch->setPlaceholderText("Search imported apps"); appSearch->setClearButtonEnabled(true); importLayout->addWidget(appSearch);
    auto *appFilter = new QComboBox; appFilter->setObjectName("appFilter"); appFilter->addItems({"All imported apps", "Running", "Failed"}); importLayout->addWidget(appFilter);
    importedEmpty = new QLabel("Import your first app: select an app on the left and click Import selected apps, or drag it into this pane. Then double-click it to launch."); importedEmpty->setObjectName("importedEmptyState"); importedEmpty->setWordWrap(true); importLayout->addWidget(importedEmpty);
    apps = new ImportedBrowser; apps->setObjectName("importedApps");
    connect(apps, &AppBrowser::visibleAppsChanged, this, [this](int count) { importedEmpty->setVisible(count == 0); importedEmpty->setText(apps->count() > 0 ? "No imported apps match this search or filter." : "Import your first app: select an app on the left and click Import selected apps, or drag it into this pane. Then double-click it to launch."); });
    connect(appSearch, &QLineEdit::textChanged, apps, &AppBrowser::setSearch);
    connect(appFilter, &QComboBox::currentIndexChanged, this, [this](int index) { apps->setFilter(index == 1 ? AppBrowser::Filter::Running : index == 2 ? AppBrowser::Filter::Failed : AppBrowser::Filter::All); });
    connect(apps, &ImportedBrowser::bundlesDropped, this, &Window::importBundles);
    connect(apps, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) { launch(item->data(Qt::UserRole).toString()); });
    importLayout->addWidget(apps);
    connect(sort, &QComboBox::currentIndexChanged, this, [this](int index) { available->setSorting(index >= 2, index % 2); apps->setSorting(index >= 2, index % 2); QSettings("cristim", "darling-launcher").setValue("appSort", index); });
    sort->setCurrentIndex(qBound(0, settings.value("appSort", 0).toInt(), 3)); available->setSorting(sort->currentIndex() >= 2, sort->currentIndex() % 2); apps->setSorting(sort->currentIndex() >= 2, sort->currentIndex() % 2);
    if (settings.value("gridView", false).toBool()) grid->click();
    auto *trashRow = new QHBoxLayout;
    trashNotice = new QLabel; trashNotice->setObjectName("trashNotice"); trashNotice->setWordWrap(true); trashRow->addWidget(trashNotice);
    undoTrash = new QPushButton("Undo"); undoTrash->setObjectName("undoTrash"); undoTrash->hide(); trashRow->addWidget(undoTrash);
    connect(undoTrash, &QPushButton::clicked, this, [this] {
        if (importRunning) { statusBar()->showMessage("Wait for the import before restoring apps."); return; }
        QList<LauncherCore::TrashReceipt> remaining;
        for (const auto &receipt : trashedApps) { QString error; if (!LauncherCore::restoreApp(receipt, &error)) { remaining << receipt; QMessageBox::warning(this, "Restore app", error); } }
        trashedApps = remaining; undoTrash->setVisible(!remaining.isEmpty()); trashNotice->setText(remaining.isEmpty() ? "Apps restored" : "Some apps could not be restored"); load();
    });
    trashRow->addStretch(); auto *trash = new TrashTarget; trash->setObjectName("appTrash"); trashRow->addWidget(trash); importLayout->addLayout(trashRow);
    connect(trash, &TrashTarget::bundlesDropped, this, [this](const QStringList &bundles) {
        if (importRunning) { QMessageBox::warning(this, "Import in progress", "Finish the current import before removing apps."); return; }
        for (const auto &key : bundles) {
            if (runningApps.contains(key)) { QMessageBox::warning(this, "App still running", "Stop the selected prefix before removing a running app."); continue; }
            QString error; LauncherCore::TrashReceipt receipt; if (!LauncherCore::trashApp(prefix->text(), key, &error, &receipt)) QMessageBox::warning(this, "App trash", error);
            else { trashedApps << receipt; undoTrash->show(); trashNotice->setText("Moved to private trash — recoverable"); pending.remove(key); failedApps.remove(key); setBusy(false, "Moved app to this prefix’s trash: " + key); }
        } load();
    });
    split->addWidget(sourcePane); split->addWidget(importPane); split->setStretchFactor(1, 2);
    layout->addWidget(split);
    split->restoreState(settings.value("splitterState").toByteArray());
    connect(apps, &QListWidget::itemSelectionChanged, this, &Window::updateContribution);
    progress = new QProgressBar; progress->setRange(0, 1); progress->setValue(0); progress->hide(); layout->addWidget(progress);
    setCentralWidget(central);
    discoverMacPartitions(); updateSourceChoices(); load();
    statusBar()->showMessage(sourceSummary->text());
    connect(prefix, &QLineEdit::textChanged, this, [this] { pending.clear(); failedApps.clear(); outputs.clear(); recoveryOutcomes.clear(); launchChoices.clear(); recoveryActions.clear(); failurePreferencesShown.clear(); recoveryReady.clear(); for (auto dialog : recoveryDialogs) if (dialog) dialog->close(); recoveryDialogs.clear(); updatePrefixChoices(); updateContribution(); });
    auto *mountRefresh = new QTimer(this); mountRefresh->setInterval(3000);
    connect(mountRefresh, &QTimer::timeout, this, &Window::updateSourceChoices); mountRefresh->start();
    if (mountAll) QTimer::singleShot(0, this, [this] {
        settingsDialog->show(); findChild<QPushButton *>("Mount macOS source…")->click();
        mountDialog->mountAllWhenReady();
    });
}
Window::~Window() {
    for (auto *process : findChildren<QProcess *>(QString(), Qt::FindDirectChildrenOnly)) process->disconnect(this);
}

void Window::updatePrefixChoices() {
    QSettings settings("cristim", "darling-launcher");
    QStringList paths = settings.value("knownPrefixes").toStringList();
    const QString managed = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/prefixes";
    for (const auto &entry : QDir(managed).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) paths << entry.absoluteFilePath();
    if (!prefix->text().isEmpty()) paths.prepend(prefix->text());
    paths.removeDuplicates(); QStringList valid;
    QSignalBlocker blocker(prefixChoices); prefixChoices->clear();
    for (const auto &path : paths) if (QDir::isAbsolutePath(path) && path != "/" && (path == prefix->text() || (QFileInfo(path).isDir() && !QFileInfo(path).isSymLink()))) {
        valid << path; prefixChoices->addItem(QFileInfo(path).fileName(), path); prefixChoices->setItemData(prefixChoices->count() - 1, path, Qt::ToolTipRole);
    }
    prefixChoices->setCurrentIndex(prefixChoices->findData(prefix->text())); prefixChoices->setToolTip(prefix->text());
    settings.setValue("knownPrefixes", valid);
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
    for (const auto &key : recoveryDialogs.keys()) updateRecovery(key);
}

void Window::updateContribution() {
    const QString key = selectedKey();
    if (troubleshoot) troubleshoot->setVisible(failedApps.contains(key) || !chains.value(key).isEmpty());
    updateRecovery(key);
}

void Window::setBusy(bool busy, const QString &message) { busy = busy || importRunning; progress->setRange(0, busy ? 0 : 1); if (!busy) progress->setValue(1); progress->setVisible(busy); statusBar()->showMessage(message); }
void Window::refresh() {
    const QString source = volume->text(); const int generation = ++scanGeneration;
    available->showPreviews({});
    if (source.isEmpty() || !QFileInfo(source).isDir()) return;
    auto *watcher = new QFutureWatcher<QList<AppPreview>>(this);
    setBusy(true, "Reading application names, icons and sizes…");
    connect(watcher, &QFutureWatcher<QList<AppPreview>>::finished, this, [this, watcher, generation, source] {
        auto previews = watcher->result(); watcher->deleteLater();
        if (generation != scanGeneration || volume->text() != source) return;
        available->showPreviews(previews);
        QStringList imported; for (const auto &entry : entries) imported << entry.sourceRelative;
        available->setImportedBundles(imported); setBusy(activeProcesses > 0 || importRunning, QString::number(previews.size()) + " apps available");
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
void Window::importDependency(const QString &key, const QString &source, const QString &destination) {
    if (prefix->text() != destination || volume->text() != source) { statusBar()->showMessage("The dependency popup belongs to a different source or prefix. Reopen it for the current selection."); return; }

    if (!pending.contains(key)) { QMessageBox::information(this, "Diagnosis", "Select an app with a missing loader dependency."); return; }
    if (runningApps.contains(key)) { QMessageBox::information(this, "Diagnosis", "The launch process is still running after the loader error. Choose Stop selected prefix processes, then retry. This stops all apps in this prefix."); return; }
    if (importRunning) { statusBar()->showMessage("An import is already in progress."); return; }
    const auto missing = pending.value(key);
    QJsonObject step{{"symbol", missing.symbol}, {"failure", missing.missingLibrary ? "missing library" : "missing symbol"}, {"library", missing.expectedIn}, {"referencedFrom", missing.referencedFrom}, {"action", "imported from selected volume"}, {"loaderOutput", outputs.value(key)}};
    step.insert("sourceVolume", source); step.insert("sourceLibrary", source + missing.expectedIn); step.insert("destinationPrefix", destination);
    auto *watcher = new QFutureWatcher<QString>(this);
    importRunning = true; setBusy(true, "Importing library…"); updateRecovery(key, "Importing the selected dependency…");
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, key, step, destination] {
        QString error = watcher->result(); watcher->deleteLater(); importRunning = false;
        if (!error.isEmpty()) {
            setBusy(activeProcesses > 0, "Library import failed: see the dependency popup.");
            updateRecovery(key, error); return;
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
            setBusy(activeProcesses > 0, "Library copied, but dependency recording failed; see popup."); updateRecovery(key, error); return;
        }
        if (prefix->text() != destination) {
            setBusy(activeProcesses > 0, "Library imported and recorded in " + destination + ". Select that prefix to retry."); return;
        }
        chains[key] = recordedChain; pending.remove(key); updateContribution(); launch(key, true);
    });
    watcher->setFuture(QtConcurrent::run([source, destination, missing] {
        QString error; LauncherCore::importLibrary(source, destination, missing.expectedIn, &error); return error;
    }));

}
void Window::importBundles(const QStringList &names) {
    if (names.isEmpty()) return;
    if (importRunning) { statusBar()->showMessage("An app import is already in progress."); return; }
    QString error;
    if (!LauncherCore::validateLocations(volume->text(), prefix->text(), &error)) { QMessageBox::warning(this, "Locations", error); return; }
    auto *watcher = new QFutureWatcher<QPair<QList<AppEntry>, QString>>(this);
    importRunning = true; setBusy(true, "Importing selected apps…");
    for (const auto &name : names) available->setActivity(name, AppBrowser::State::Queued);
    available->setActivity(names.first(), AppBrowser::State::Importing);
    const QString v = volume->text(), p = prefix->text();
    connect(watcher, &QFutureWatcher<QPair<QList<AppEntry>, QString>>::finished, this, [this, watcher, p, names] {
        for (const auto &name : names) available->setActivity(name, AppBrowser::State::Ready);
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
    connect(watcher, &QFutureWatcherBase::progressValueChanged, this, [this, v, names](int completed) {
        if (volume->text() != v) return;
        progress->setRange(0, names.size()); progress->setValue(completed);
        for (int index = 0; index < names.size(); ++index) available->setActivity(names[index], index < completed ? AppBrowser::State::Ready : index == completed ? AppBrowser::State::Importing : AppBrowser::State::Queued);
    });
    watcher->setFuture(QtConcurrent::run([v, p, names](QPromise<QPair<QList<AppEntry>, QString>> &promise) {
        promise.setProgressRange(0, names.size());
        QList<AppEntry> result; QString error;
        for (const QString &name : names) {
            AppEntry entry;
            if (!LauncherCore::importApp(v, p, name, &entry, &error)) break;
            result << entry;
            promise.setProgressValue(result.size());
        }
        promise.addResult(qMakePair(result, error));
    }));
}
void Window::load() {
    updatePrefixChoices();
    entries.clear(); chains.clear();
    auto catalog = LauncherCore::loadCatalog(prefix->text());
    for (const auto &item : catalog.value("apps").toArray()) {
        auto obj = item.toObject(); AppEntry entry{obj.value("name").toString(), obj.value("bundle").toString(), obj.value("executable").toString(), obj.value("source").toString()};
        if (!entry.relativeBundle.isEmpty()) { entries.insert(entry.relativeBundle, entry); chains.insert(entry.relativeBundle, obj.value("chain").toArray()); }
    }
    QList<AppPreview> previews;
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        auto preview = LauncherCore::appPreview(prefix->text(), it.key());
        if (preview.relativeBundle.isEmpty()) preview = {it.key(), it.value().name, {}, 0};
        previews << preview;
    }
    apps->showPreviews(previews);
    QStringList imported; for (const auto &entry : entries) imported << entry.sourceRelative;
    available->setImportedBundles(imported);
    for (const auto &key : entries.keys()) {
        apps->setActivity(key, failedApps.contains(key) ? AppBrowser::State::Failed : runningApps.contains(key) ? AppBrowser::State::Running : AppBrowser::State::Ready);
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
QString Window::selectedKey() const { return apps->currentItem() ? apps->currentItem()->data(Qt::UserRole).toString() : QString(); }
bool Window::confirmLaunch(const QString &key, bool failed) {
    auto saved = LauncherTroubleshooting::recoveryChoices();
    if (!saved.remember) {
        LaunchChoicesDialog choices(entries.value(key).name, hasMountedSource(), this, failed);
        choices.setSourceAvailability(hasMountedSource(), hasPossibleMacOSSource());
        connect(&choices, &LaunchChoicesDialog::mountRequested, this, [this] { findChild<QPushButton *>("Mount macOS source…")->click(); });
        connect(volume, &QLineEdit::textChanged, &choices, [this, &choices] { choices.setSourceAvailability(hasMountedSource(), hasPossibleMacOSSource()); });
        if (choices.exec() != QDialog::Accepted) return false;
        saved = choices.choices(); LauncherTroubleshooting::saveRecoveryChoices(saved);
    }
    launchChoices[key] = saved;
    if (!failed) recoveryActions[key].clear();
    if (failed || saved.remember) recoveryReady.insert(key); else recoveryReady.remove(key);
    return true;
}
void Window::launch(const QString &key, bool retry) {
    if (!entries.contains(key)) return;
    if (runningApps.contains(key)) { setBusy(true, "This app already has a running launch process."); return; }
    if (!retry && !confirmLaunch(key)) return;
    failurePreferencesShown.remove(key);
    if (!LauncherTroubleshooting::recoveryChoices().remember) recoveryReady.remove(key);
    pending.remove(key);
    const AppEntry app = entries.value(key);
    outputs[key].clear(); recoveryOutcomes.remove(key);
    failedApps.remove(key); apps->setActivity(key, AppBrowser::State::Launching);
    updateContribution();
    runCommand(app.name, {"exec", "/" + app.relativeBundle + "/Contents/MacOS/" + app.executable}, key);
}
void Window::runCommand(const QString &label, const QStringList &args, const QString &key) {
    auto invalid = [this, label, key](const QString &error) {
        if (!key.isEmpty()) { outputs[key] = error; failedApps.insert(key); apps->setActivity(key, AppBrowser::State::Failed); apps->setStatus(key, "Failed to start"); updateContribution(); }
        setBusy(activeProcesses > 0, error); QMessageBox::warning(this, label, error);
        if (!key.isEmpty()) recoverFailure(key);
    };
    if (!QFileInfo(darling->text()).isExecutable()) { invalid("Select an executable Darling host launcher."); return; }
    QString p = prefix->text();
    if (!QDir::isAbsolutePath(p) || !QFileInfo(p).isDir()) { invalid("Select an existing prefix directory."); return; }
    if (!runtimeRoot->text().isEmpty() && (!QDir::isAbsolutePath(runtimeRoot->text()) || !QFileInfo(runtimeRoot->text()).isDir())) { invalid("Select a valid runtime install root."); return; }
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
    connect(process, &QProcess::started, this, [this, key, p] { if (!key.isEmpty() && prefix->text() == p) apps->setActivity(key, AppBrowser::State::Running); });
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process, key, p] {
        QString text = QString::fromLocal8Bit(process->readAllStandardOutput());
        if (!key.isEmpty() && prefix->text() == p) { outputs[key] += text; diagnoseOutput(key); updateRecovery(key); }
    });
    connect(process, &QProcess::finished, this, [this, process, key, label, p](int code, QProcess::ExitStatus exit) {
        const bool success = code == 0 && exit == QProcess::NormalExit;
        --activeProcesses;
        runningApps.remove(key);
        setBusy(activeProcesses > 0, label + " exited with status " + QString::number(code));
        if (!key.isEmpty() && prefix->text() == p) {
            outputs[key] += QString::fromLocal8Bit(process->readAllStandardOutput());
            MissingSymbol missing = LauncherCore::diagnose(outputs.value(key));

            if (missing.valid() && !success) {
                diagnoseOutput(key);
            } else {
                apps->setActivity(key, success ? AppBrowser::State::Exited : AppBrowser::State::Failed);
                apps->setStatus(key, success ? "Exited successfully" : "Exited " + QString::number(code));
                if (!success) failedApps.insert(key);
                if (success && !chains.value(key).isEmpty())
                    setBusy(activeProcesses > 0, label + " succeeded after importing macOS libraries. Review the dependency popup to help VibeDarling.");
            }
        }
        updateContribution(); updateRecovery(key, apps->status(key));
        if (!key.isEmpty() && prefix->text() == p && !success) recoverFailure(key);
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process, label, key, p](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) { --activeProcesses; runningApps.remove(key); if (!key.isEmpty() && prefix->text() == p) { outputs[key] += process->errorString(); failedApps.insert(key); apps->setActivity(key, AppBrowser::State::Failed); apps->setStatus(key, "Failed to start"); } setBusy(activeProcesses > 0, label + ": " + process->errorString()); updateContribution(); if (!key.isEmpty() && prefix->text() == p) recoverFailure(key); process->deleteLater(); }
    });
    process->start(darling->text(), args);
}

void Window::diagnoseOutput(const QString &key) {
    if (pending.contains(key)) return;
    const auto missing = LauncherCore::diagnose(outputs.value(key));
    if (!missing.valid()) return;
    pending.insert(key, missing);
    failedApps.insert(key); apps->setActivity(key, AppBrowser::State::Failed); apps->setStatus(key, missing.missingLibrary ? "Needs " + QFileInfo(missing.expectedIn).fileName() : "Missing " + missing.symbol);
    setBusy(activeProcesses > 0, "Launch needs a dependency. Choose recovery actions in the popup.");
    updateContribution();
    recoverFailure(key);
}

void Window::recoverFailure(const QString &key) {
    if (!entries.contains(key) || failurePreferencesShown.contains(key)) return;
    failurePreferencesShown.insert(key);
    const QString destination = prefix->text();
    QTimer::singleShot(0, this, [this, key, destination] {
        if (prefix->text() != destination || !failedApps.contains(key)) return;
        if (!confirmLaunch(key, true)) return;
        if (prefix->text() == destination && failedApps.contains(key)) showRecovery(key);
    });
}

void Window::discoverMacPartitions() {
    if (partitionDiscovery && partitionDiscovery->state() != QProcess::NotRunning) return;
    const QString tool = QStandardPaths::findExecutable("lsblk"); if (tool.isEmpty()) return;
    if (!partitionDiscovery) {
        partitionDiscovery = new QProcess(this);
        connect(partitionDiscovery, &QProcess::finished, this, [this](int code, QProcess::ExitStatus exit) {
            QString error;
            const auto partitions = LauncherMount::parsePartitions(partitionDiscovery->readAllStandardOutput(), &error);
            if (code != 0 || exit != QProcess::NormalExit || !error.isEmpty()) return;
            macPartitionsPossible = !partitions.isEmpty();
            for (auto *dialog : findChildren<LaunchChoicesDialog *>()) dialog->setSourceAvailability(hasMountedSource(), hasPossibleMacOSSource());
            for (const auto &key : recoveryDialogs.keys()) updateRecovery(key);
        });
    }
    partitionDiscovery->start(tool, {"--json", "--output", "PATH,FSTYPE,LABEL,MOUNTPOINTS"});
    QTimer::singleShot(5000, partitionDiscovery, [this] { if (partitionDiscovery->state() != QProcess::NotRunning) partitionDiscovery->kill(); });
}
bool Window::hasPossibleMacOSSource() const {
    if (macPartitionsPossible) return true;
    for (const auto &candidate : LauncherSources::candidates(mountProvider())) if (candidate.usable()) return true;
    return false;
}

bool Window::hasMountedSource() const {
    const QString selected = QFileInfo(volume->text()).canonicalFilePath();
    if (selected.isEmpty()) return false;
    for (const auto &candidate : LauncherSources::candidates(mountProvider()))
        if (candidate.usable() && QFileInfo(candidate.root).canonicalFilePath() == selected) return true;
    return false;
}
QJsonObject Window::diagnostic(const QString &key) const {
    const auto app = entries.value(key); const auto missing = pending.value(key);
    return QJsonObject{{"app", app.name}, {"bundle", key}, {"executable", app.executable}, {"sourceBundle", app.sourceRelative}, {"prefix", prefix->text()}, {"sourceVolume", volume->text()}, {"sourceMounted", hasMountedSource()}, {"sourceAvailable", hasPossibleMacOSSource()}, {"launcher", darling->text()}, {"runtime", runtimeRoot->text()}, {"loaderOutput", outputs.value(key)}, {"dependencyChain", chains.value(key)}, {"missingLibrary", missing.expectedIn}, {"missingSymbol", missing.symbol}};
}
void Window::updateRecovery(const QString &key, const QString &result) {
    auto *dialog = recoveryDialogs.value(key).data(); if (!dialog || !entries.contains(key)) return;
    const auto missing = pending.value(key);
    const bool running = runningApps.contains(key);
    const bool source = hasMountedSource();
    if (!result.isEmpty()) recoveryOutcomes[key] = result;
    QString outcome = recoveryOutcomes.value(key);
    if (!chains.value(key).isEmpty()) outcome = "Copied macOS libraries into this private prefix and recorded their source and loader provenance. " + (outcome.isEmpty() ? apps->status(key) : outcome) + ". A local workaround helps reproduce the missing functionality; a source fix remains separate.";
    dialog->updateDiagnostic(diagnostic(key), missing.valid() && source && !running && !importRunning, running, outcome);
    QTimer::singleShot(0, this, [this, key] { dispatchRecovery(key); });
}
void Window::dispatchRecovery(const QString &key) {
    auto *dialog = recoveryDialogs.value(key).data();
    if (!dialog || !failedApps.contains(key) || !recoveryReady.contains(key)) return;
    const auto preferences = launchChoices.value(key); auto &done = recoveryActions[key];
    const QString library = "import:" + pending.value(key).expectedIn;
    const bool copy = pending.value(key).valid() && preferences.importLibrary && !done.contains(library) && hasMountedSource() && !runningApps.contains(key) && !importRunning;
    const bool report = preferences.report && !done.contains("report");
    const bool ai = preferences.ai && !done.contains("ai");
    if (copy) done.insert(library); if (report) done.insert("report"); if (ai) done.insert("ai");
    if (copy || report || ai) dialog->chooseActions(copy, report, ai, preferences.agent);
}
void Window::showRecovery(const QString &key) {
    if (!entries.contains(key)) return;
    auto *dialog = recoveryDialogs.value(key).data();
    if (!dialog) {
        dialog = new TroubleshootingDialog(diagnostic(key), this); dialog->setAttribute(Qt::WA_DeleteOnClose); recoveryDialogs[key] = dialog;
        const QString destination = prefix->text();
        connect(dialog, &TroubleshootingDialog::importRequested, this, [this, key, destination] { if (hasMountedSource()) importDependency(key, volume->text(), destination); });
        connect(dialog, &TroubleshootingDialog::mountRequested, this, [this] {
            for (auto *button : findChildren<QPushButton *>()) if (button->text() == "Mount macOS source…") { button->click(); break; }
        });
        connect(dialog, &TroubleshootingDialog::stopRequested, this, [this, destination] { if (prefix->text() == destination) runCommand("Stop prefix", {"shutdown"}); });
    }
    updateRecovery(key); dialog->show(); dialog->raise();
}
void Window::openTroubleshooting() {
    const QString key = selectedKey();
    if (!entries.contains(key) || (!failedApps.contains(key) && chains.value(key).isEmpty())) return;
    showRecovery(key);
}

void Window::closeEvent(QCloseEvent *event) {
    for (auto *dialog : findChildren<QDialog *>("issueApprovalDialog"))
        for (auto *process : dialog->findChildren<QProcess *>()) if (process->state() != QProcess::NotRunning) { statusBar()->showMessage("Wait for the approved issue submission to finish before closing."); event->ignore(); return; }
    for (auto *job : findChildren<BackgroundFix *>()) if (job->isRunning()) { statusBar()->showMessage("A source-fix agent is running. Finish it or use Settings → Stop background agents before closing."); event->ignore(); return; }
    if (importRunning) { statusBar()->showMessage("Finish the current import and catalog update before closing the launcher."); event->ignore(); return; }
    if (mountDialog && mountDialog->isMounting()) {
        statusBar()->showMessage("Complete or dismiss the authentication prompt before closing the launcher.");
        event->ignore(); return;
    }
    QSettings settings("cristim", "darling-launcher");
    settings.setValue("windowGeometry", saveGeometry()); settings.setValue("splitterState", browserSplitter->saveState());
    settings.setValue("volume", volume->text()); settings.setValue("prefix", prefix->text()); settings.setValue("darling", darling->text()); settings.setValue("runtimeRoot", runtimeRoot->text());
    QMainWindow::closeEvent(event);
}
