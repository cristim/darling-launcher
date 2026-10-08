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
struct FixVerification {
    bool verified = false;
    QString reason;
};
class FixVerifier : public QObject {
    Q_OBJECT
public:
    FixVerifier(const QString &workspace, const QJsonObject &diagnostic, int stableSeconds, QObject *parent = nullptr);
    void start();
    bool isRunning() const { return running; }
signals:
    void finished(const FixVerification &result);
private:
    void finish(bool verified, const QString &reason);
    QString workspace, launcher, runtimeRoot, proposalCommit;
    QJsonObject diagnostic;
    int stableSeconds;
    QProcess process;
    QString output;
    bool running = false;
};
class BackgroundFix : public QObject {
    Q_OBJECT
public:
    BackgroundFix(const QString &agent, const QStringList &args, const QString &workspace, QObject *parent, const QJsonObject &diagnostic = {});
    ~BackgroundFix() override;
    bool isRunning() const;
public slots:
    void stop();
signals:
    void statusChanged(const QString &message);
    void verified(const QString &proposalFile);
    void verificationOffered();
public slots:
    void verify();
private:
    QString workspacePath;
    QJsonObject failure;
    FixVerifier *verifier = nullptr;
    QProcess process;
    bool waiting = true;
};
struct PrProposal {
    QJsonObject fields;
    QString patch, commit, error;
    bool valid() const { return error.isEmpty() && !patch.isEmpty(); }
};
namespace LauncherTroubleshooting {
QString verificationFile(const QString &workspace, const QString &commit);
QStringList agents();
QStringList agentArguments(const QString &agent, const QString &prompt);
struct AgentAccess { QStringList directories; QString launcher; };
QStringList backgroundArguments(const QString &agent, const QString &prompt, const AgentAccess &access = {});
PrProposal reviewProposal(const QString &file);
}
struct RecoveryChoices {
    bool importLibrary = false, report = false, ai = false, remember = false;
    QString agent;
};
class LaunchChoicesDialog : public QDialog {
    Q_OBJECT
public:
    LaunchChoicesDialog(const QString &app, bool mounted, QWidget *parent = nullptr, bool failed = false);
    RecoveryChoices choices() const;
    void setMounted(bool mounted);
    void setSourceAvailability(bool mounted, bool possible);
signals:
    void mountRequested();
private:
    QCheckBox *copy, *report, *ai, *remember;
    QComboBox *agent;
    QPushButton *mount;
    bool macOSPossible = true;
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
    void actionsChosen(bool copy, bool report, bool ai, const QString &agent);
    void importRequested();
    void mountRequested();
    void stopRequested();
    void backgroundStatus(const QString &message);
private:
    void submitProposal(const PrProposal &proposal);
    QString issueBody() const;
    void updateIssueDrafts();
    bool applyingChoices = false;
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
