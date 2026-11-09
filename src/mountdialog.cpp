// SPDX-License-Identifier: GPL-3.0-or-later
#include "mountdialog.h"
#include "core.h"
#include "discovery.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QSettings>
#include <QSharedPointer>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStandardItemModel>
#include <QTextEdit>
#include <QVBoxLayout>
#include <sys/stat.h>
#include <unistd.h>

namespace {
// Test builds run user-owned fake helpers; production accepts only root-owned ones.
unsigned helperOwner() {
#ifdef LAUNCHER_TEST_USER_OWNED_HELPERS
    return getuid();
#else
    return 0;
#endif
}
QString installHint() {
    return "Root runs the cryptex mount script, so it must be installed root-owned. Either run\n"
           "  sudo cmake --install <build directory>\n"
           "or copy the two files by hand:\n"
           "  sudo install -D -o root -g root -m 0755 tools/mount-cryptex.py " LAUNCHER_MOUNT_CRYPTEX_SCRIPT "\n"
           "  sudo install -o root -g root -m 0644 tools/mount-macos.py " + QFileInfo(QStringLiteral(LAUNCHER_MOUNT_CRYPTEX_SCRIPT)).absolutePath() + "/mount-macos.py\n"
           "Manual alternative from a terminal: mount Preboot as root with apfs-fuse (see the README), then run the os.dmg mount as yourself.";
}
}

MountDialog::MountDialog(QWidget *parent, std::function<QList<SourceMount>()> provider) : QDialog(parent), mountProvider(std::move(provider)) {
    setWindowTitle("Mount macOS source — read-only"); resize(650, 440);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel("Select a partition to reuse its existing mounted volumes.\npkexec requests authorization for a read-only mount under your launcher folder."));
    auto *form = new QFormLayout;
    partitions = new QComboBox; partitions->setObjectName("mountPartitions"); form->addRow("Partition", partitions);
    backend = new QComboBox; backend->addItems({"Kernel filesystem driver", "APFS FUSE"}); form->addRow("Driver", backend);
    volumeIndex = new QSpinBox; volumeIndex->setRange(-1, 2147483647); volumeIndex->setValue(-1); volumeIndex->setSpecialValueText("Select a volume index"); form->addRow("APFS volume index", volumeIndex);
    directory = new QLineEdit; directory->setObjectName("mountDirectory");
    directory->setParent(this); directory->hide();
    ownedMountChoices = new QComboBox; form->addRow("Session mounts (select to unmount)", ownedMountChoices);
    connect(ownedMountChoices, &QComboBox::activated, this, [this](int index) {
        ownedMount = ownedMountChoices->itemText(index); ownedDevice = ownedMountChoices->itemData(index).toByteArray(); unmountButton->setEnabled(!ownedMount.isEmpty());
    });
    layout->addLayout(form);
    existingSources = new QComboBox; existingSources->setObjectName("existingPartitionSources"); form->addRow("Existing mounted volumes", existingSources);
    useSource = new QPushButton("Use existing source"); useSource->setObjectName("useExistingSource"); layout->addWidget(useSource);
    connect(existingSources, &QComboBox::currentIndexChanged, this, [this] { useSource->setEnabled(existingSources->currentIndex() > 0); });
    connect(partitions, &QComboBox::currentIndexChanged, this, &MountDialog::updateExistingSources);
    connect(useSource, &QPushButton::clicked, this, [this] {
        QString root = existingSources->currentData().toString();
        int index = partitions->currentData().toInt(); QString device = index >= 0 && index < detected.size() ? detected[index].device : QString();
        for (const auto &candidate : LauncherSources::candidates(mountProvider(), device)) if (candidate.root == root && candidate.usable()) { emit sourceMounted(root); output->append("Reusing existing mounted source: " + root); return; }
        output->append("The selected source is no longer mounted or suitable."); updateExistingSources();
    });
    auto *fusePath = new QLineEdit(QSettings("cristim", "darling-launcher").value("apfsFuse", LauncherDiscovery::helperExecutable(LauncherDiscovery::roots(), "apfs-fuse")).toString());
    auto *utilityPath = new QLineEdit(QSettings("cristim", "darling-launcher").value("apfsUtil", LauncherDiscovery::helperExecutable(LauncherDiscovery::roots(), "apfsutil")).toString());
    fusePath->setObjectName("apfsFuseField"); utilityPath->setObjectName("apfsUtilField");
    auto *batchRoot = new QLineEdit(LauncherDiscovery::dataRoot() + "/mounts");
    for (QLineEdit *internal : {fusePath, utilityPath, batchRoot}) { internal->setParent(this); internal->hide(); }
    connect(fusePath, &QLineEdit::editingFinished, this, [=] { QSettings("cristim", "darling-launcher").setValue("apfsFuse", fusePath->text()); });
    connect(utilityPath, &QLineEdit::editingFinished, this, [=] { QSettings("cristim", "darling-launcher").setValue("apfsUtil", utilityPath->text()); });
    layout->addWidget(new QLabel("Kernel APFS requires an installed APFS module; FUSE requires apfs-fuse.\nEncrypted APFS volumes require an external unlock workflow."));
    auto *actions = new QHBoxLayout;
    auto *refresh = new QPushButton("Refresh partitions"); refresh->setObjectName("refreshPartitions"); actions->addWidget(refresh); connect(refresh, &QPushButton::clicked, this, &MountDialog::discover);
    mountButton = new QPushButton("Mount read-only"); mountButton->setObjectName("mountReadOnly"); actions->addWidget(mountButton); connect(mountButton, &QPushButton::clicked, this, &MountDialog::mountSelected);
    unmountButton = new QPushButton("Unmount this session's source"); unmountButton->setEnabled(false); actions->addWidget(unmountButton);
    connect(unmountButton, &QPushButton::clicked, this, [this] {
        QStorageInfo storage(ownedMount); storage.refresh();
        if (ownedMount.isEmpty() || storage.rootPath() != ownedMount || storage.device() != ownedDevice) { output->append("The tracked source is no longer mounted at this directory."); return; }
        QString pkexec = QStandardPaths::findExecutable("pkexec"), umount = QStandardPaths::findExecutable("umount");
        if (pkexec.isEmpty() || umount.isEmpty()) { output->append("pkexec or umount is unavailable."); return; }
        execute({pkexec, {umount, "--", ownedMount}}, true);
    });
    auto *all = new QPushButton("Mount all detected volumes read-only"); all->setObjectName("mountAllVolumes"); actions->addWidget(all);
    connect(all, &QPushButton::clicked, this, [=] {
        if (detected.isEmpty()) { output->append("No macOS partitions detected. Refresh first."); return; }
        const QString python = QStandardPaths::findExecutable("python3");
        const QString script = QStringLiteral(LAUNCHER_MOUNT_SCRIPT);
        if (python.isEmpty() || !QFileInfo(script).isFile()) { output->append("Python or the single-authorization mount script is unavailable."); return; }
        auto *batch = new QProcess(this); batch->setProcessChannelMode(QProcess::MergedChannels);
        mountBusy = true; all->setEnabled(false); mountButton->setEnabled(false);
        output->append("Waiting for one authorization request for the entire read-only batch…");
        auto buffer = QSharedPointer<QByteArray>::create();
        auto mounts = QSharedPointer<QStringList>::create();
        auto consume = [=] {
            *buffer += batch->readAllStandardOutput();
            int newline;
            while ((newline = buffer->indexOf('\n')) >= 0) {
                QByteArray line = buffer->left(newline); buffer->remove(0, newline + 1);
                qInfo().noquote() << QString::fromUtf8(line);
                auto event = QJsonDocument::fromJson(line).object();
                QString type = event.value("event").toString();
                if (type == "mounted") {
                    QString path = event.value("path").toString(); QStorageInfo storage(path); storage.refresh();
                    if (storage.rootPath() == path && storage.isReadOnly() && storage.device() == event.value("device").toString().toUtf8()) {
                        *mounts << path;
                        if (ownedMountChoices->findText(path) < 0) ownedMountChoices->addItem(path, storage.device());
                    }
                    output->append("Mounted read-only: " + path);
                } else if (type == "skip") output->append(event.value("device").toString() + ": " + event.value("reason").toString());
                else if (!event.value("message").toString().isEmpty()) output->append(event.value("message").toString());
                else if (event.isEmpty()) output->append(QString::fromUtf8(line));
            }
        };
        connect(batch, &QProcess::readyReadStandardOutput, this, consume);
        connect(batch, &QProcess::finished, this, [=](int code, QProcess::ExitStatus exit) {
            consume(); mountBusy = false;
            batchMounts += *mounts; all->setEnabled(true); mountButton->setEnabled(true);
            if (!mounts->isEmpty()) { ownedMount = mounts->first(); ownedDevice = QStorageInfo(ownedMount).device(); ownedMountChoices->setCurrentText(ownedMount); unmountButton->setEnabled(true); }
            output->append(exit != QProcess::NormalExit ? "Mount helper crashed." : LauncherMount::exitDescription(code));
            auto sources = LauncherCore::mountedMacVolumes();
            if (sources.size() == 1) emit sourceMounted(sources.first());
            else if (sources.size() > 1) for (const auto &source : sources) output->append("macOS applications source (select in Settings): " + source);
            else output->append("No mounted macOS applications source found.");
            batch->deleteLater();
            discover();
        });
        connect(batch, &QProcess::errorOccurred, this, [=](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) { mountBusy = false; output->append(batch->errorString()); all->setEnabled(true); mountButton->setEnabled(true); batch->deleteLater(); }
        });
        batch->start(python, {"-I", script, "--mount-root", batchRoot->text(), "--apfs-fuse", fusePath->text(), "--apfsutil", utilityPath->text()});
    });
    cryptexButton = new QPushButton("Mount cryptex OS (Preboot + os.dmg)"); cryptexButton->setObjectName("mountCryptexOs"); actions->addWidget(cryptexButton);
    connect(cryptexButton, &QPushButton::clicked, this, [=] { mountCryptex(batchRoot->text(), fusePath->text()); });
    cryptexUnmountButton = new QPushButton("Unmount cryptex OS"); cryptexUnmountButton->setObjectName("unmountCryptexOs"); cryptexUnmountButton->setEnabled(false); actions->addWidget(cryptexUnmountButton);
    connect(cryptexUnmountButton, &QPushButton::clicked, this, &MountDialog::unmountCryptex);
    prebootUnmountButton = new QPushButton("Unmount Preboot (authorization)"); prebootUnmountButton->setObjectName("unmountPreboot"); actions->addWidget(prebootUnmountButton);
    connect(prebootUnmountButton, &QPushButton::clicked, this, &MountDialog::unmountPreboot);
    layout->addLayout(actions);
    auto *cryptexNote = new QLabel("The cryptex action mounts Preboot with one authorization request (select a partition to pin it), then mounts os.dmg as you.\nThe privileged step only uses root-owned apfs-fuse and apfsutil from /usr/local/bin or /usr/bin and mounts Preboot under /run/darling-launcher/<uid>.\napfs-fuse can use several GB of memory while reading it: unmount the cryptex OS first, then Preboot, if the system slows down.");
    cryptexNote->setWordWrap(true); layout->addWidget(cryptexNote);
    output = new QTextEdit; output->setReadOnly(true); output->setObjectName("mountOutput"); layout->addWidget(output);
    if (QString problem; !LauncherMount::trustedCryptexScript(QStringLiteral(LAUNCHER_MOUNT_CRYPTEX_SCRIPT), helperOwner(), &problem)) {
        cryptexButton->setEnabled(false); prebootUnmountButton->setEnabled(false); output->append("Cryptex OS mount is disabled: " + problem + "\n" + installHint());
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close); connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject); layout->addWidget(buttons);
    connect(&discovery, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        QString previousDevice; int previous = partitions->currentData().toInt(); if (previous >= 0 && previous < detected.size()) previousDevice = detected[previous].device;
        partitions->clear(); partitions->addItem("Select a partition", -1);
        if (code != 0 || status != QProcess::NormalExit) { output->append("Partition discovery failed: " + QString::fromLocal8Bit(discovery.readAllStandardError())); return; }
        QString error; detected = LauncherMount::parsePartitions(discovery.readAllStandardOutput(), &error);
        if (!error.isEmpty()) { output->append(error); return; }
        for (int i = 0; i < detected.size(); ++i) {
            const auto &part = detected[i];
            partitions->addItem(part.device + " — " + part.filesystem + " " + part.label + (part.mounted ? " (" + QString::number(part.mountPoints.size()) + " mounts)" : ""), i);
            if (part.device == previousDevice) partitions->setCurrentIndex(i + 1);
        }
        updateExistingSources();
        if (detected.isEmpty()) output->append("No APFS or HFS partitions found.");
        if (requestedBatch) { requestedBatch = false; findChild<QPushButton *>("mountAllVolumes")->click(); }
    });
    connect(&discovery, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) output->append(discovery.errorString());
    });
    discover();
}
void MountDialog::updateExistingSources() {
    if (!existingSources || !mountButton) return;
    const QString previous = existingSources->currentData().toString();
    int index = partitions->currentData().isValid() ? partitions->currentData().toInt() : -1;
    QString device = index >= 0 && index < detected.size() ? detected[index].device : QString();
    const auto candidates = LauncherSources::candidates(mountProvider(), device);
    existingSources->clear(); existingSources->addItem("Choose a usable existing source…", QString());
    int usable = 0; QStringList usableRoots;
    for (const auto &candidate : candidates) {
        existingSources->addItem(candidate.description(), candidate.root);
        if (candidate.usable()) { ++usable; usableRoots << candidate.root; }
        else if (auto *model = qobject_cast<QStandardItemModel *>(existingSources->model())) model->item(existingSources->count() - 1)->setEnabled(false);
    }
    if (usable == 0) existingSources->setItemText(0, candidates.isEmpty() ? "No existing mounted volumes" : "No usable sources — mounted volumes have no app/library directories");
    QString chosen = LauncherSources::automaticSource(candidates, previous);
    int selected = usableRoots.contains(chosen) ? existingSources->findData(chosen) : 0; existingSources->setCurrentIndex(selected < 0 ? 0 : selected);
    useSource->setEnabled(existingSources->currentIndex() > 0);
    bool mounted = !candidates.isEmpty() || (index >= 0 && index < detected.size() && detected[index].mounted);
    mountButton->setEnabled(!mountBusy && !mounted && !device.isEmpty());
    if (output && !device.isEmpty() && mounted) output->append(device + ": " + QString::number(candidates.size()) + " existing volumes, " + QString::number(usable) + " usable sources. Existing mounts will be reused.");
}
void MountDialog::discover() {
    if (discovery.state() != QProcess::NotRunning) return;
    QString tool = QStandardPaths::findExecutable("lsblk");
    if (tool.isEmpty()) { output->append("lsblk is unavailable."); return; }
    discovery.start(tool, {"--json", "--output", "PATH,FSTYPE,LABEL,MOUNTPOINTS"});
}
void MountDialog::mountSelected() {
    int selected = partitions->currentData().toInt();
    if (selected < 0 || selected >= detected.size()) { output->append("Select a detected partition."); return; }
    const auto &partition = detected[selected];
    for (const auto &mounted : QStorageInfo::mountedVolumes())
        if (QFileInfo(QString::fromLocal8Bit(mounted.device())).canonicalFilePath() == QFileInfo(partition.device).canonicalFilePath()) {
            output->append("The selected partition is already mounted. Refresh mounted-volume discovery."); return;
        }
    struct stat deviceStatus {};
    if (::stat(QFile::encodeName(partition.device).constData(), &deviceStatus) != 0 || !S_ISBLK(deviceStatus.st_mode)) { output->append("Selected device is not a block device."); return; }
    directory->setText(LauncherDiscovery::dataRoot() + "/mounts/" + QFileInfo(partition.device).fileName() + "-manual");
    if (!QDir().mkpath(directory->text())) { output->append("Cannot create " + directory->text()); return; }
    QStorageInfo storage(directory->text()); storage.refresh();
    if (storage.rootPath() == QFileInfo(directory->text()).canonicalFilePath()) { output->append("The mount directory is already a mount point."); return; }
    MountBackend driver = backend->currentIndex() == 1 ? MountBackend::ApfsFuse : MountBackend::Kernel;
    QString tool = driver == MountBackend::ApfsFuse ? QSettings("cristim", "darling-launcher").value("apfsFuse", LauncherDiscovery::helperExecutable(LauncherDiscovery::roots(), "apfs-fuse")).toString() : QStandardPaths::findExecutable("mount");
    MountCommand command; QString error;
    if (!LauncherMount::makeCommand(partition, directory->text(), driver, volumeIndex->value(), getuid(), getgid(),
                                    QStandardPaths::findExecutable("pkexec"), tool, &command, &error)) { output->append(error); return; }
    execute(command, false, partition.device);
}
bool MountDialog::cryptexCommandReady(QString *python) {
    QString error;
    *python = LauncherMount::trustedFile(LauncherMount::launcherPython(), helperOwner(), true, &error);
    if (!python->isEmpty() && LauncherMount::trustedCryptexScript(QStringLiteral(LAUNCHER_MOUNT_CRYPTEX_SCRIPT), helperOwner(), &error)) return true;
    cryptexButton->setEnabled(false); prebootUnmountButton->setEnabled(false);
    output->append(error + "\n" + installHint() + "\nThe root-run interpreter (" + LauncherMount::launcherPython() + ") was chosen when the launcher was configured; reconfigure with -DLAUNCHER_PYTHON=<trusted python 3.8+> if it is missing or not root-owned.");
    return false;
}

void MountDialog::mountCryptex(const QString &requestedRoot, const QString &apfsFuse) {
    const QString root = LauncherMount::resolveHomePrefix(requestedRoot, LauncherMount::passwdHome());
    if (root.isEmpty() && !requestedRoot.isEmpty()) { output->append("Your home directory passes through a dangling symlink; fix it before mounting."); return; }
    const int selected = partitions->currentData().toInt();
    const QString device = selected >= 0 && selected < detected.size() ? detected[selected].device : QString();
    MountCommand command; QString error, python;
    if (!cryptexCommandReady(&python)) return;
    if (!LauncherMount::makeCryptexCommand(python, QStringLiteral(LAUNCHER_MOUNT_CRYPTEX_SCRIPT),
                                           root, apfsFuse, device, &command, &error)) { output->append(error); return; }
    output->append("Warning: apfs-fuse can use several GB of memory while reading the cryptex image.");
    pendingCryptexRoot = root;
    runCryptexCommand(command, "Waiting for one authorization request for the Preboot mount…");
}

void MountDialog::unmountPreboot() {
    MountCommand command; QString error, python;
    if (!cryptexCommandReady(&python)) return;
    if (!LauncherMount::makePrebootUnmountCommand(python, QStringLiteral(LAUNCHER_MOUNT_CRYPTEX_SCRIPT), &command, &error)) { output->append(error); return; }
    output->append("Unmount the cryptex OS first if it is mounted (Preboot stays busy while it is).");
    runCryptexCommand(command, "Waiting for one authorization request to unmount Preboot…");
}

void MountDialog::runCryptexCommand(const MountCommand &command, const QString &waiting) {
    auto *process = new QProcess(this); process->setProcessChannelMode(QProcess::MergedChannels);
    auto setBusy = [this](bool busy) { mountBusy = busy; cryptexButton->setEnabled(!busy); prebootUnmountButton->setEnabled(!busy); mountButton->setEnabled(!busy); };
    setBusy(true);
    output->append(waiting);
    auto buffer = QSharedPointer<QByteArray>::create();
    auto consume = [=] {
        *buffer += process->readAllStandardOutput();
        int newline;
        while ((newline = buffer->indexOf('\n')) >= 0) { handleCryptexEvent(buffer->left(newline)); buffer->remove(0, newline + 1); }
    };
    connect(process, &QProcess::readyReadStandardOutput, this, consume);
    connect(process, &QProcess::finished, this, [=](int code, QProcess::ExitStatus exit) {
        consume(); setBusy(false); pendingCryptexRoot.clear();
        output->append(exit != QProcess::NormalExit ? "Cryptex mount helper crashed." : LauncherMount::exitDescription(code));
        process->deleteLater(); discover();
    });
    connect(process, &QProcess::errorOccurred, this, [=](QProcess::ProcessError failure) {
        if (failure == QProcess::FailedToStart) { setBusy(false); output->append(process->errorString()); process->deleteLater(); }
    });
    process->start(command.program, command.arguments);
}

void MountDialog::handleCryptexEvent(const QByteArray &line) {
    qInfo().noquote() << QString::fromUtf8(line);
    const auto event = QJsonDocument::fromJson(line).object();
    const QString type = event.value("event").toString(), path = event.value("path").toString(), role = event.value("role").toString();
    if (type == "mounted") {
        QStorageInfo storage(path); storage.refresh();
        const bool verified = storage.rootPath() == path;
        if (role == "cryptex-os" && verified) {
            cryptexRoot = LauncherMount::adoptCryptexRoot(cryptexRoot, pendingCryptexRoot, path);
            cryptexUnmountButton->setEnabled(!cryptexRoot.isEmpty());
        }
        output->append((verified ? "Mounted read-only: " : "Reported mounted but not verified: ") + path);
    } else if (type == "unmounted") output->append("Unmounted " + role + ": " + path);
    else if (type == "dyld") output->append("dyld shared cache directory: " + path);
    else if (type == "warning") output->append("Warning: " + event.value("message").toString());
    else if (!event.value("message").toString().isEmpty()) output->append(event.value("message").toString());
    else if (event.isEmpty() && !line.isEmpty()) output->append(QString::fromUtf8(line));
}

void MountDialog::unmountCryptex() {
    MountCommand command; QString error;
    QString fusermount = QStandardPaths::findExecutable("fusermount3");
    if (fusermount.isEmpty()) fusermount = QStandardPaths::findExecutable("fusermount");
    if (!LauncherMount::makeCryptexUnmountCommand(fusermount, cryptexRoot, &command, &error)) { output->append(error); return; }
    auto *runner = new MountRunner(this);
    const QString target = LauncherMount::cryptexMountDirectory(cryptexRoot);
    cryptexUnmountButton->setEnabled(false);
    connect(runner, &MountRunner::output, output, &QTextEdit::insertPlainText);
    connect(runner, &MountRunner::completed, this, [this, runner, target](bool, const QString &message) {
        output->append(message);
        QStorageInfo storage(target); storage.refresh();
        if (storage.rootPath() == target) { output->append("The cryptex OS is still mounted: " + target); cryptexUnmountButton->setEnabled(true); }
        else output->append("Cryptex OS unmounted. Unmount Preboot from the session mounts when finished.");
        runner->deleteLater();
    });
    runner->start(command);
}

void MountDialog::execute(const MountCommand &command, bool unmount, const QString &device) {
    auto *runner = new MountRunner(this);
    const QString target = unmount ? ownedMount : QFileInfo(directory->text()).canonicalFilePath();
    mountButton->setEnabled(false); unmountButton->setEnabled(false);
    output->append(unmount ? "Waiting for unmount authorization…" : "Waiting for read-only mount authorization…");
    connect(runner, &MountRunner::output, output, &QTextEdit::insertPlainText);
    connect(runner, &MountRunner::completed, this, [this, runner, unmount, target, device](bool success, const QString &message) {
        output->append(message); mountButton->setEnabled(true);
        if (success) {
            QStorageInfo storage(target); storage.refresh();
            if (unmount) {
                if (storage.rootPath() != target) {
                    ownedMount.clear(); int index = ownedMountChoices->findText(target); if (index >= 0) ownedMountChoices->removeItem(index);
                    if (ownedMountChoices->count()) { ownedMount = ownedMountChoices->currentText(); ownedDevice = ownedMountChoices->currentData().toByteArray(); }
                } else output->append("The directory is still mounted.");
            } else if (storage.rootPath() == target && storage.isReadOnly() && QFileInfo(QString::fromLocal8Bit(storage.device())).canonicalFilePath() == QFileInfo(device).canonicalFilePath()) {
                ownedMount = target; ownedDevice = storage.device();
                if (ownedMountChoices->findText(target) < 0) ownedMountChoices->addItem(target, ownedDevice);
                if (LauncherCore::looksLikeMacVolume(target)) emit sourceMounted(target);
                else output->append("Mounted, but no macOS applications layout was found. Choose the appropriate APFS volume index.");
            } else output->append("The selected directory was not verified as a read-only mount.");
        }
        unmountButton->setEnabled(!ownedMount.isEmpty()); runner->deleteLater(); discover();
    });
    runner->start(command);
}

void MountDialog::mountAllWhenReady() {
    if (discovery.state() != QProcess::NotRunning) requestedBatch = true;
    else findChild<QPushButton *>("mountAllVolumes")->click();
}

void MountDialog::reject() {
    if (mountBusy) { output->append("Complete or dismiss the authentication prompt before closing the mount workflow."); return; }
    QDialog::reject();
}
