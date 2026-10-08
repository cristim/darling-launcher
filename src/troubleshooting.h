// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDialog>
#include <QJsonObject>
#include <QStringList>
class QLabel;
class QPushButton;
struct PrProposal {
    QJsonObject fields;
    QString patch, commit, error;
    bool valid() const { return error.isEmpty() && !patch.isEmpty(); }
};
namespace LauncherTroubleshooting {
QStringList agents();
QStringList agentArguments(const QString &agent, const QString &prompt);
PrProposal reviewProposal(const QString &file);
}
class TroubleshootingDialog : public QDialog {
    Q_OBJECT
public:
    explicit TroubleshootingDialog(const QJsonObject &diagnostic, QWidget *parent = nullptr);
public slots:
    void loadProposal(const QString &file);
private:
    void submitProposal(const PrProposal &proposal);
    QString gh;
    bool authenticated = false;
    QLabel *status;
    QPushButton *review;
};
