// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core.h"
#include "sources.h"
#include <functional>
#include <QJsonArray>
#include <QMainWindow>
#include <QMap>
#include <QProcess>
#include <QSet>

class QComboBox;
class QCloseEvent;
class QLabel;
class QWidget;
class QLineEdit;
class QListWidget;
class QTableWidget;
class QPushButton;
class QTextEdit;
class QProgressBar;
class MountDialog;
class AppBrowser;
class ImportTable;
class PrefixDialog;

class Window : public QMainWindow {
public:
    explicit Window(const QString &builderScript = {}, bool mountAll = false, std::function<QList<SourceMount>()> mountProvider = LauncherSources::mounts);
    ~Window() override;
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    std::function<QList<SourceMount>()> mountProvider;
    QComboBox *sourceChoices;
    QLabel *sourceSummary;
    QString sourceSignature;
    bool sourceChoicesInitialized = false;
    QLineEdit *volume;
    QLineEdit *prefix;
    QLineEdit *darling;
    QLineEdit *runtimeRoot;
    AppBrowser *available;
    ImportTable *apps;
    QTextEdit *log;
    QWidget *contributionPanel;
    QLabel *contributionMessage;
    QPushButton *libraryRetry;
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
    void launch(const QString &key);
    void runCommand(const QString &label, const QStringList &args, const QString &key = {});
    QString selectedKey() const;
    void setBusy(bool busy, const QString &message);
};
