// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "mount.h"
#include "sources.h"
#include <functional>
#include <QDialog>

class QComboBox;
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
    QSpinBox *volumeIndex;
    QPushButton *mountButton = nullptr;
    QPushButton *unmountButton;
    QPushButton *allButton = nullptr;
    QPushButton *cryptexButton = nullptr;
    QPushButton *cryptexUnmountButton = nullptr;
    QPushButton *prebootUnmountButton = nullptr;
    QString cryptexRoot;         // root of the cryptex mount that was verified; Unmount targets this
    QString pendingCryptexRoot;  // root of the attempt in progress
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
    void runHelper(const QStringList &selection);
    void unmount(const MountCommand &command);
    void mountCryptex(const QString &root, const QString &apfsFuse);
    void runCryptexCommand(const MountCommand &command, const QString &waiting);
    bool cryptexCommandReady(QString *python);
    void unmountPreboot();
    void handleCryptexEvent(const QByteArray &line);
    void unmountCryptex();
};
