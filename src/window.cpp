#include "window.h"
#include "mountdialog.h"
#include "appbrowser.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QGridLayout>
#include <QButtonGroup>
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
#include <QStatusBar>
#include <QSplitter>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QtConcurrent>

Window::Window() {
    setWindowTitle("Darling Launcher — host application");
    resize(1050, 720);
    auto *central = new QWidget;
    auto *layout = new QVBoxLayout(central);
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
    volume->setObjectName("volumeField"); prefix->setObjectName("prefixField"); darling->setObjectName("darlingField");
    layout->addLayout(form);
    QSettings settings("cristim", "darling-launcher");
    volume->setText(settings.value("volume").toString());
    prefix->setText(settings.value("prefix").toString());
    darling->setText(settings.value("darling").toString());
    for (QLineEdit *field : {volume, prefix, darling})
        connect(field, &QLineEdit::editingFinished, this, [this] {
            QSettings settings("cristim", "darling-launcher");
            settings.setValue("volume", volume->text()); settings.setValue("prefix", prefix->text()); settings.setValue("darling", darling->text());
        });
    auto *toolbar = new QGridLayout;
    int actionCount = 0;
    auto add = [&](const QString &title, auto callback) { auto *button = new QPushButton(title); button->setObjectName(title); toolbar->addWidget(button, actionCount / 4, actionCount % 4); ++actionCount; connect(button, &QPushButton::clicked, this, callback); };
    add("Create prefix", [this] {
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
        QStringList mounts = LauncherCore::mountedMacVolumes();
        if (mounts.isEmpty()) { setBusy(false, "No mounted macOS volume found. Use Mount macOS source to select a partition."); return; }
        QMenu menu(this);
        for (const QString &mount : mounts) menu.addAction(mount, this, [this, mount] { volume->setText(mount); refresh(); });
        menu.exec(QCursor::pos());
    });
    add("Mount macOS source…", [this] {
        if (!mountDialog) {
            mountDialog = new MountDialog(this);
            connect(mountDialog, &MountDialog::sourceMounted, this, [this](const QString &path) {
                volume->setText(path); refresh(); setBusy(false, "Mounted macOS source: " + path);
                QSettings("cristim", "darling-launcher").setValue("volume", path);
            });
        }
        mountDialog->show(); mountDialog->raise(); mountDialog->activateWindow();
    });
    add("Import selected apps", [this] { importBundles(available->selectedBundles()); });
    add("Launch selected", [this] { QString key = selectedKey(); if (!key.isEmpty()) launch(key); });
    add("Import needed library and retry", [this] {
        QString key = selectedKey();
        if (!pending.contains(key)) { QMessageBox::information(this, "Diagnosis", "Select an app with a loader symbol failure."); return; }
        QString error; auto missing = pending.value(key);
        if (!LauncherCore::importLibrary(volume->text(), prefix->text(), missing.expectedIn, &error)) {
            QMessageBox::warning(this, "Library import", error); return;
        }
        QJsonObject step{{"symbol", missing.symbol}, {"library", missing.expectedIn}, {"referencedFrom", missing.referencedFrom}, {"action", "imported from selected volume"}};
        chains[key].append(step); persist(); pending.remove(key); launch(key);
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
    auto *bottom = new QHBoxLayout;
    auto *draft = new QPushButton("Save proposed issue"); bottom->addWidget(draft);
    connect(draft, &QPushButton::clicked, this, [this] {
        QString key = selectedKey(); if (!entries.contains(key)) return;
        QString path = prefix->text() + "/.darling-launcher/proposed-vibedarling-issue.md";
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) { QMessageBox::warning(this, "Draft", file.errorString()); return; }
        file.write(LauncherCore::issueDraft(entries.value(key), chains.value(key), outputs.value(key), volume->text(), prefix->text(), darling->text()).toUtf8());
        if (!file.commit()) { QMessageBox::warning(this, "Draft", file.errorString()); return; }
        QMessageBox::information(this, "Draft saved", path + "\nReview and approve before any submission.");
    });
    auto *fix = new QPushButton("Opt in: source fix workflow"); bottom->addWidget(fix);
    connect(fix, &QPushButton::clicked, this, [this] {
        QMessageBox::information(this, "Separate source fix workflow", "After reviewing the issue draft, opt in separately to a clean-room VibeDarling source change. Work from published source, headers, documentation and API observations only. Prepare a patch, tests and PR draft for your review; no PR is submitted automatically.");
    });
    layout->addLayout(bottom);
    progress = new QProgressBar; progress->setRange(0, 1); progress->setValue(0); layout->addWidget(progress);
    log = new QTextEdit; log->setReadOnly(true); layout->addWidget(log);
    setCentralWidget(central);
    refresh(); load();
    if (LauncherCore::mountedMacVolumes().isEmpty())
        statusBar()->showMessage("No mounted macOS volume found. Select a mounted volume when available.");
}

Window::~Window() {
    for (auto *process : findChildren<QProcess *>(QString(), Qt::FindDirectChildrenOnly)) process->disconnect(this);
}

void Window::setBusy(bool busy, const QString &message) { progress->setRange(0, busy ? 0 : 1); if (!busy) progress->setValue(1); statusBar()->showMessage(message); log->append(message); }
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
}
void Window::persist() {
    QJsonArray array;
    for (auto it = entries.cbegin(); it != entries.cend(); ++it)
        array.append(QJsonObject{{"name", it.value().name}, {"bundle", it.key()}, {"executable", it.value().executable}, {"source", it.value().sourceRelative}, {"chain", chains.value(it.key())}});
    QString error;
    if (!LauncherCore::saveCatalog(prefix->text(), QJsonObject{{"apps", array}}, &error)) QMessageBox::warning(this, "Catalog", error);
}
QString Window::selectedKey() const { int row = apps->currentRow(); return row < 0 ? QString() : apps->item(row, 1)->text(); }
void Window::launch(const QString &key) {
    if (!entries.contains(key)) return;
    const AppEntry app = entries.value(key);
    outputs[key].clear();
    runCommand(app.name, {"exec", "/" + app.relativeBundle + "/Contents/MacOS/" + app.executable}, key);
}
void Window::runCommand(const QString &label, const QStringList &args, const QString &key) {
    if (!QFileInfo(darling->text()).isExecutable()) { QMessageBox::warning(this, label, "Select an executable Darling host launcher."); return; }
    QString p = prefix->text();
    if (!QDir::isAbsolutePath(p) || !QFileInfo(p).isDir()) { QMessageBox::warning(this, label, "Select an existing prefix directory."); return; }
    auto *process = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment(); env.remove("DYLD_LIBRARY_PATH"); env.remove("DYLD_INSERT_LIBRARIES"); env.insert("DPREFIX", p);
    process->setProcessEnvironment(env); process->setProcessChannelMode(QProcess::MergedChannels);
    ++activeProcesses;
    setBusy(true, "Running " + label);
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process, key] {
        QString text = QString::fromLocal8Bit(process->readAllStandardOutput()); log->insertPlainText(text); log->ensureCursorVisible();
        if (!key.isEmpty()) outputs[key] += text;
    });
    connect(process, &QProcess::finished, this, [this, process, key, label](int code, QProcess::ExitStatus) {
        --activeProcesses;
        setBusy(activeProcesses > 0, label + " exited with status " + QString::number(code));
        if (!key.isEmpty()) {
            outputs[key] += QString::fromLocal8Bit(process->readAllStandardOutput());
            MissingSymbol missing = LauncherCore::diagnose(outputs.value(key));
            int row = -1; for (int i = 0; i < apps->rowCount(); ++i) if (apps->item(i, 1)->text() == key) row = i;
            if (missing.valid() && code != 0) {
                pending.insert(key, missing);
                if (row >= 0) apps->item(row, 2)->setText("Missing " + missing.symbol + " in " + missing.expectedIn);
                QMessageBox::warning(this, "Library needed", missing.symbol + "\nExpected in: " + missing.expectedIn + "\nSelect the app and choose Import needed library and retry.");
            } else if (row >= 0) {
                apps->item(row, 2)->setText(code == 0 ? "Exited successfully" : "Exited " + QString::number(code));
                if (code == 0 && !chains.value(key).isEmpty()) QMessageBox::information(this, "Launch succeeded", "The dependency chain for " + entries.value(key).name + " is recorded. Save the proposed issue for review.");
            }
        }
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process, label](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) { --activeProcesses; setBusy(activeProcesses > 0, label + ": " + process->errorString()); process->deleteLater(); }
    });
    process->start(darling->text(), args);
}
