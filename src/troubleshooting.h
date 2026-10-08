// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDialog>
#include <QJsonObject>
#include <QStringList>
#include <QProcess>
class QLabel;
class QPushButton;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QTextEdit;
class BackgroundFix : public QObject {
    Q_OBJECT
public:
    BackgroundFix(const QString &agent, const QStringList &args, const QString &workspace, QObject *parent);
    ~BackgroundFix() override;
    bool isRunning() const;
public slots:
    void stop();
signals:
    void statusChanged(const QString &message);
private:
    QProcess process;
    bool waiting = true;
};
struct PrProposal {
    QJsonObject fields;
    QString patch, commit, error;
    bool valid() const { return error.isEmpty() && !patch.isEmpty(); }
};
namespace LauncherTroubleshooting {
QStringList agents();
QStringList agentArguments(const QString &agent, const QString &prompt);
QStringList backgroundArguments(const QString &agent, const QString &prompt);
PrProposal reviewProposal(const QString &file);
}
struct RecoveryChoices {
    bool importLibrary = false, report = false, ai = false, remember = false;
    QString agent;
};
class LaunchChoicesDialog : public QDialog {
    Q_OBJECT
public:
    LaunchChoicesDialog(const QString &app, bool mounted, QWidget *parent = nullptr);
    RecoveryChoices choices() const;
    void setMounted(bool mounted);
signals:
    void mountRequested();
private:
    QCheckBox *copy, *report, *ai, *remember;
    QComboBox *agent;
    QPushButton *mount;
};
namespace LauncherTroubleshooting {
RecoveryChoices recoveryChoices();
void saveRecoveryChoices(const RecoveryChoices &choices);
}
class TroubleshootingDialog : public QDialog {
    Q_OBJECT
public:
    explicit TroubleshootingDialog(const QJsonObject &diagnostic, QWidget *parent = nullptr);
    ~TroubleshootingDialog() override;
public slots:
    void done(int result) override;
    void loadProposal(const QString &file);
    void reviewIssue();
    void chooseActions(bool importLibrary, bool report, bool ai, const QString &agent);
    void updateDiagnostic(const QJsonObject &data, bool canImport, bool launchRunning, const QString &result = {});
signals:
    void importRequested();
    void mountRequested();
    void stopRequested();
    void backgroundStatus(const QString &message);
private:
    void submitProposal(const PrProposal &proposal);
    QJsonObject diagnostic;
    QCheckBox *importOption = nullptr, *issueOption = nullptr, *aiOption = nullptr;
    QTextEdit *details;
    QLabel *summary, *outcome;
    QPushButton *stopPrefix = nullptr, *mountSource = nullptr;
    QString gh;
    bool authenticated = false;
    QLabel *status;
    QPushButton *review;
};
