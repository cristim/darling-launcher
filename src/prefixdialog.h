// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDialog>
class PrefixDialog : public QDialog {
    Q_OBJECT
public:
    explicit PrefixDialog(const QString &sourceVolume, const QString &scriptOverride = {}, QWidget *parent = nullptr);
    ~PrefixDialog() override;
    bool isBusy() const;
    void startAutomatically();
    void done(int result) override;
private:
    bool automatic = false;
signals:
    void finished();
    void failed(const QString &message);
    void prefixReady(const QString &prefix, const QString &launcher, const QString &runtime);
};
