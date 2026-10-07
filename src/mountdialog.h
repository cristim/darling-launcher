// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "mount.h"
#include "sources.h"
#include <functional>
#include <QDialog>

class QComboBox;
class QLineEdit;
class QSpinBox;
class QPushButton;
class QTextEdit;

class MountDialog : public QDialog {
    Q_OBJECT
public:
    explicit MountDialog(QWidget *parent = nullptr, std::function<QList<SourceMount>()> mountProvider = LauncherSources::mounts);
    void mountAllWhenReady();
    bool isMounting() const { return mountBusy; }
    void reject() override;
signals:
    void sourceMounted(const QString &directory);
private:
    std::function<QList<SourceMount>()> mountProvider;
    QComboBox *existingSources = nullptr;
    QPushButton *useSource;
    QComboBox *partitions;
    QComboBox *backend;
    QComboBox *ownedMountChoices;
    QLineEdit *directory;
    QSpinBox *volumeIndex;
    QPushButton *mountButton = nullptr;
    QPushButton *unmountButton;
    QTextEdit *output = nullptr;
    bool mountBusy = false;
    bool requestedBatch = false;
    QList<MacPartition> detected;
    QProcess discovery;
    QStringList batchMounts;
    QString ownedMount;
    QByteArray ownedDevice;
    void updateExistingSources();
    void discover();
    void mountSelected();
    void execute(const MountCommand &command, bool unmount, const QString &device = {});
};
