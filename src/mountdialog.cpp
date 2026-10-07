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
#include <QTextEdit>
#include <QVBoxLayout>
#include <sys/stat.h>
#include <unistd.h>

MountDialog::MountDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle("Mount macOS source — read-only"); resize(650, 440);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel("Select a detected partition and an empty mount directory.\nAuthentication uses pkexec. Mounts are read-only, nodev, nosuid and noexec."));
    auto *form = new QFormLayout;
    partitions = new QComboBox; partitions->setObjectName("mountPartitions"); form->addRow("Partition", partitions);
    backend = new QComboBox; backend->addItems({"Kernel filesystem driver", "APFS FUSE"}); form->addRow("Driver", backend);
    volumeIndex = new QSpinBox; volumeIndex->setRange(-1, 2147483647); volumeIndex->setValue(-1); volumeIndex->setSpecialValueText("Select a volume index"); form->addRow("APFS volume index", volumeIndex);
    directory = new QLineEdit; directory->setObjectName("mountDirectory");
    auto *row = new QHBoxLayout; row->addWidget(directory);
    auto *browse = new QPushButton("Browse…"); row->addWidget(browse);
    connect(browse, &QPushButton::clicked, this, [this] {
        QString path = QFileDialog::getExistingDirectory(this, "Choose an empty mount directory");
        if (!path.isEmpty()) directory->setText(path);
    });
    form->addRow("Mount directory", row);
    ownedMountChoices = new QComboBox; form->addRow("Session mounts (select to unmount)", ownedMountChoices);
    connect(ownedMountChoices, &QComboBox::activated, this, [this](int index) {
        ownedMount = ownedMountChoices->itemText(index); ownedDevice = ownedMountChoices->itemData(index).toByteArray(); unmountButton->setEnabled(!ownedMount.isEmpty());
    });
    layout->addLayout(form);
    auto *fusePath = new QLineEdit(QSettings("cristim", "darling-launcher").value("apfsFuse", LauncherDiscovery::helperExecutable(LauncherDiscovery::roots(), "apfs-fuse")).toString());
    auto *utilityPath = new QLineEdit(QSettings("cristim", "darling-launcher").value("apfsUtil", LauncherDiscovery::helperExecutable(LauncherDiscovery::roots(), "apfsutil")).toString());
    auto *batchRoot = new QLineEdit(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/mounts");
    form->addRow("APFS FUSE executable", fusePath); form->addRow("APFS metadata utility", utilityPath); form->addRow("Batch mount root", batchRoot);
    connect(fusePath, &QLineEdit::editingFinished, this, [=] { QSettings("cristim", "darling-launcher").setValue("apfsFuse", fusePath->text()); });
    connect(utilityPath, &QLineEdit::editingFinished, this, [=] { QSettings("cristim", "darling-launcher").setValue("apfsUtil", utilityPath->text()); });
    layout->addWidget(new QLabel("Kernel APFS requires an installed APFS module; FUSE requires apfs-fuse.\nEncrypted APFS volumes require an external unlock workflow."));
    auto *actions = new QHBoxLayout;
    auto *refresh = new QPushButton("Refresh partitions"); actions->addWidget(refresh); connect(refresh, &QPushButton::clicked, this, &MountDialog::discover);
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
        auto *batch = new MountBatch(this); all->setEnabled(false); mountButton->setEnabled(false);
        connect(batch, &MountBatch::output, output, &QTextEdit::append);
        connect(batch, &MountBatch::completed, this, [=](const QStringList &mounts, bool cancelled) {
            batchMounts += mounts; all->setEnabled(true); mountButton->setEnabled(true);
            for (const auto &path : mounts) if (ownedMountChoices->findText(path) < 0) ownedMountChoices->addItem(path, QStorageInfo(path).device());
            if (!mounts.isEmpty()) { ownedMount = mounts.first(); ownedDevice = QStorageInfo(ownedMount).device(); ownedMountChoices->setCurrentText(ownedMount); unmountButton->setEnabled(true); }
            output->append(cancelled ? "Stopped after authorization was cancelled or denied." : "Finished scanning detected volumes.");
            auto sources = LauncherCore::mountedMacVolumes();
            if (sources.size() == 1) emit sourceMounted(sources.first());
            else if (sources.size() > 1) {
                partitions->clear(); partitions->addItem("Multiple sources found; use Detect mounted macOS volumes in Settings", -1);
                for (const auto &source : sources) output->append("macOS applications source: " + source);
            } else output->append("No mounted volume with macOS applications was found.");
            batch->deleteLater();
        });
        batch->start(detected, batchRoot->text(), QStandardPaths::findExecutable("pkexec"), QStandardPaths::findExecutable("mount"), fusePath->text(), utilityPath->text());
    });
    layout->addLayout(actions);
    output = new QTextEdit; output->setReadOnly(true); output->setObjectName("mountOutput"); layout->addWidget(output);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close); connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject); layout->addWidget(buttons);
    connect(&discovery, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        partitions->clear(); partitions->addItem("Select a partition", -1);
        if (code != 0 || status != QProcess::NormalExit) { output->append("Partition discovery failed: " + QString::fromLocal8Bit(discovery.readAllStandardError())); return; }
        QString error; detected = LauncherMount::parsePartitions(discovery.readAllStandardOutput(), &error);
        if (!error.isEmpty()) { output->append(error); return; }
        for (int i = 0; i < detected.size(); ++i) {
            const auto &part = detected[i];
            partitions->addItem(part.device + " — " + part.filesystem + " " + part.label + (part.mounted ? " (mounted)" : ""), i);
        }
        if (detected.isEmpty()) output->append("No APFS or HFS partitions found.");
    });
    connect(&discovery, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) output->append(discovery.errorString());
    });
    discover();
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
    QStorageInfo storage(directory->text()); storage.refresh();
    if (storage.rootPath() == QFileInfo(directory->text()).canonicalFilePath()) { output->append("The mount directory is already a mount point."); return; }
    MountBackend driver = backend->currentIndex() == 1 ? MountBackend::ApfsFuse : MountBackend::Kernel;
    QString tool = driver == MountBackend::ApfsFuse ? QSettings("cristim", "darling-launcher").value("apfsFuse", LauncherDiscovery::helperExecutable(LauncherDiscovery::roots(), "apfs-fuse")).toString() : QStandardPaths::findExecutable("mount");
    MountCommand command; QString error;
    if (!LauncherMount::makeCommand(partition, directory->text(), driver, volumeIndex->value(), getuid(), getgid(),
                                    QStandardPaths::findExecutable("pkexec"), tool, &command, &error)) { output->append(error); return; }
    execute(command, false, partition.device);
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
