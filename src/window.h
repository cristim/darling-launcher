// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core.h"
#include "sources.h"
#include "troubleshooting.h"
#include <functional>
#include <QJsonArray>
#include <QMainWindow>
#include <QMap>
#include <QProcess>
#include <QSet>
#include <QPointer>

class QComboBox;
class QCloseEvent;
class QLabel;
class QWidget;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTextEdit;
class QProgressBar;
class MountDialog;
class AppBrowser;
class ImportedBrowser;
class QDialog;
class QSplitter;
class PrefixDialog;
class TroubleshootingDialog;

class Window : public QMainWindow {
public:
    explicit Window(const QString &builderScript = {}, bool mountAll = false, std::function<QList<SourceMount>()> mountProvider = LauncherSources::mounts);
    ~Window() override;
    void offerRuntimeSetup();
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    std::function<QList<SourceMount>()> mountProvider;
    QComboBox *sourceChoices;
    QLabel *sourceSummary;
    QString sourceSignature;
    bool sourceChoicesInitialized = false;
    QString prefixBuilderScript;
    void openRuntimeBuilder(bool fresh = false);
    QLineEdit *volume;
    QLineEdit *prefix;
    QLineEdit *darling;
    QLineEdit *runtimeRoot;
    AppBrowser *available;
    QLabel *sourceEmpty;
    QLabel *importedEmpty;
    ImportedBrowser *apps;
    QDialog *settingsDialog;
    QSplitter *browserSplitter;
    QComboBox *prefixChoices;
    void updatePrefixChoices();
    QSet<QString> failedApps;
    QList<LauncherCore::TrashReceipt> trashedApps;
    QLabel *trashNotice;
    QPushButton *undoTrash;
    QMap<QString, QPointer<TroubleshootingDialog>> recoveryDialogs;
    QPushButton *troubleshoot = nullptr;
    QMap<QString, RecoveryChoices> launchChoices;
    QMap<QString, QString> recoveryOutcomes;
    QMap<QString, QSet<QString>> recoveryActions;
    void dispatchRecovery(const QString &key);
    bool confirmLaunch(const QString &key, bool failed = false);
    void recoverFailure(const QString &key);
    QSet<QString> failurePreferencesShown, recoveryReady;
    bool hasMountedSource() const;
    bool hasPossibleMacOSSource() const;
    void discoverMacPartitions();
    QProcess *partitionDiscovery = nullptr;
    bool macPartitionsPossible = true;
    QJsonObject diagnostic(const QString &key) const;
    void showRecovery(const QString &key);
    void updateRecovery(const QString &key, const QString &result = {});
    void importDependency(const QString &key, const QString &source, const QString &destination);
    void openTroubleshooting();
    QProgressBar *progress;
    MountDialog *mountDialog = nullptr;
    PrefixDialog *prefixDialog = nullptr;
    QMap<QString, AppEntry> entries;
    int activeProcesses = 0;
    int scanGeneration = 0;
    bool importRunning = false;
    QMap<QString, QJsonArray> chains;
    QMap<QString, QString> outputs;
    QMap<QString, MissingSymbol> pending;
    QSet<QString> runningApps;
    void diagnoseOutput(const QString &key);
    void updateSourceChoices();
    void updateContribution();
    void refresh();
    void importBundles(const QStringList &names);
    void load();
    void persist();
    void launch(const QString &key, bool retry = false);
    void runCommand(const QString &label, const QStringList &args, const QString &key = {});
    QString selectedKey() const;
    void setBusy(bool busy, const QString &message);
};
