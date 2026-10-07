// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "mount.h"
#include <QDialog>

class QComboBox;
class QLineEdit;
class QSpinBox;
class QPushButton;
class QTextEdit;

class MountDialog : public QDialog {
    Q_OBJECT
public:
    explicit MountDialog(QWidget *parent = nullptr);
signals:
    void sourceMounted(const QString &directory);
private:
    QComboBox *partitions;
    QComboBox *backend;
    QLineEdit *directory;
    QSpinBox *volumeIndex;
    QPushButton *mountButton;
    QPushButton *unmountButton;
    QTextEdit *output;
    QList<MacPartition> detected;
    QProcess discovery;
    QString ownedMount;
    QByteArray ownedDevice;
    void discover();
    void mountSelected();
    void execute(const MountCommand &command, bool unmount, const QString &device = {});
};
