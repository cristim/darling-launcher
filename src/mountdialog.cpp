// SPDX-License-Identifier: GPL-3.0-or-later
#include "mountdialog.h"
#include "core.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStorageInfo>
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

#ifndef LAUNCHER_PYTHON
#define LAUNCHER_PYTHON "/usr/bin/python3"
#endif

namespace {
// Test builds run user-owned fake helpers; production accepts only root-owned ones.
unsigned helperOwner() {
#ifdef LAUNCHER_TEST_USER_OWNED_HELPERS
    return getuid();
#else
    return 0;
#endif
}
}

MountDialog::MountDialog(QWidget *parent, std::function<QList<SourceMount>()> provider) : QDialog(parent), mountProvider(std::move(provider)) {
    setWindowTitle("Mount macOS source — read-only"); resize(650, 440);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel("Select a partition to reuse its existing mounted volumes.\npkexec requests authorization for a read-only mount under /run/darling-launcher."));
    auto *form = new QFormLayout;
    partitions = new QComboBox; partitions->setObjectName("mountPartitions"); form->addRow("Partition", partitions);
    backend = new QComboBox; backend->addItems({"Kernel filesystem driver", "APFS FUSE"}); form->addRow("Driver", backend);
    volumeIndex = new QSpinBox; volumeIndex->setRange(-1, 99); volumeIndex->setValue(-1); volumeIndex->setSpecialValueText("Select a volume index"); form->addRow("APFS volume index", volumeIndex);
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
    layout->addWidget(new QLabel("Kernel APFS requires an installed APFS module; FUSE requires apfs-fuse.\nEncrypted APFS volumes require an external unlock workflow."));
    auto *actions = new QHBoxLayout;
    auto *refresh = new QPushButton("Refresh partitions"); refresh->setObjectName("refreshPartitions"); actions->addWidget(refresh); connect(refresh, &QPushButton::clicked, this, &MountDialog::discover);
    mountButton = new QPushButton("Mount read-only"); mountButton->setObjectName("mountReadOnly"); actions->addWidget(mountButton); connect(mountButton, &QPushButton::clicked, this, &MountDialog::mountSelected);
    unmountButton = new QPushButton("Unmount this session's source"); unmountButton->setEnabled(false); actions->addWidget(unmountButton);
    connect(unmountButton, &QPushButton::clicked, this, [this] {
        QStorageInfo storage(ownedMount); storage.refresh();
        if (ownedMount.isEmpty() || storage.rootPath() != ownedMount || storage.device() != ownedDevice) { output->append("The tracked source is no longer mounted at this directory."); return; }
        if (!LauncherMount::isHelperMount(ownedMount)) { output->append("Only mounts made by the launcher's mount helper can be unmounted here."); return; }
        QString error; const QString pkexec = LauncherMount::trustedExecutable(LauncherMount::pkexecPath, 0, &error), umount = LauncherMount::trustedExecutable(LauncherMount::umountPath, 0, &error);
        if (pkexec.isEmpty() || umount.isEmpty()) { output->append(error); return; }
        unmount({pkexec, {umount, "--", ownedMount}});
    });
    allButton = new QPushButton("Mount all detected volumes read-only"); allButton->setObjectName("mountAllVolumes"); actions->addWidget(allButton);
    connect(allButton, &QPushButton::clicked, this, [this] {
        if (detected.isEmpty()) { output->append("No macOS partitions detected. Refresh first."); return; }
        runHelper({});
    });
    layout->addLayout(actions);
    output = new QTextEdit; output->setReadOnly(true); output->setObjectName("mountOutput"); layout->addWidget(output);
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
    QStringList selection{"--device=" + partition.device};
    if (volumeIndex->value() >= 0) selection << "--volume=" + QString::number(volumeIndex->value());
    if (backend->currentIndex() == 0) selection << "--kernel";
    runHelper(selection);
}
void MountDialog::runHelper(const QStringList &selection) {
    QString error;
    const QString python = LauncherMount::trustedExecutable(QStringLiteral(LAUNCHER_PYTHON), helperOwner(), &error);
    const QString script = python.isEmpty() ? QString() : LauncherMount::trustedExecutable(QStringLiteral(LAUNCHER_MOUNT_SCRIPT), helperOwner(), &error);
    if (script.isEmpty()) {
        output->append(error + "\nThe mount helper runs as root, so it must be installed root-owned at " LAUNCHER_MOUNT_SCRIPT " (sudo cmake --install <build directory>).");
        return;
    }
    auto *batch = new QProcess(this); batch->setProcessChannelMode(QProcess::MergedChannels);
    mountBusy = true; allButton->setEnabled(false); mountButton->setEnabled(false);
    output->append(selection.isEmpty() ? "Waiting for one authorization request for the entire read-only batch…" : "Waiting for read-only mount authorization…");
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
        batchMounts += *mounts; allButton->setEnabled(true); mountButton->setEnabled(true);
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
        if (error == QProcess::FailedToStart) { mountBusy = false; output->append(batch->errorString()); allButton->setEnabled(true); mountButton->setEnabled(true); batch->deleteLater(); }
    });
    batch->start(python, QStringList{"-I", script} + selection);
}
void MountDialog::unmount(const MountCommand &command) {
    auto *runner = new MountRunner(this);
    const QString target = ownedMount;
    mountButton->setEnabled(false); unmountButton->setEnabled(false);
    output->append("Waiting for unmount authorization…");
    connect(runner, &MountRunner::output, output, &QTextEdit::insertPlainText);
    connect(runner, &MountRunner::completed, this, [this, runner, target](bool success, const QString &message) {
        output->append(message); mountButton->setEnabled(true);
        if (success) {
            QStorageInfo storage(target); storage.refresh();
            if (storage.rootPath() != target) {
                ownedMount.clear(); int index = ownedMountChoices->findText(target); if (index >= 0) ownedMountChoices->removeItem(index);
                if (ownedMountChoices->count()) { ownedMount = ownedMountChoices->currentText(); ownedDevice = ownedMountChoices->currentData().toByteArray(); }
            } else output->append("The directory is still mounted.");
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
