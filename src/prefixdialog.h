// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDialog>
class PrefixDialog : public QDialog {
    Q_OBJECT
public:
    explicit PrefixDialog(const QString &sourceVolume, const QString &scriptOverride = {}, QWidget *parent = nullptr);
    ~PrefixDialog() override;
signals:
    void prefixReady(const QString &prefix, const QString &launcher, const QString &runtime);
};
