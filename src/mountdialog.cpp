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
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStorageInfo>
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
    form->addRow("Mount directory", row); layout->addLayout(form);
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
    QString tool = QStandardPaths::findExecutable(driver == MountBackend::ApfsFuse ? "apfs-fuse" : "mount");
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
                if (storage.rootPath() != target) ownedMount.clear(); else output->append("The directory is still mounted.");
            } else if (storage.rootPath() == target && storage.isReadOnly() && QFileInfo(QString::fromLocal8Bit(storage.device())).canonicalFilePath() == QFileInfo(device).canonicalFilePath()) {
                ownedMount = target; ownedDevice = storage.device();
                if (LauncherCore::looksLikeMacVolume(target)) emit sourceMounted(target);
                else output->append("Mounted, but no macOS applications layout was found. Choose the appropriate APFS volume index.");
            } else output->append("The selected directory was not verified as a read-only mount.");
        }
        unmountButton->setEnabled(!ownedMount.isEmpty()); runner->deleteLater(); discover();
    });
    runner->start(command);
}
