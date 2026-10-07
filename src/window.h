#pragma once
#include "core.h"
#include <QJsonArray>
#include <QMainWindow>
#include <QMap>
#include <QProcess>

class QLineEdit;
class QListWidget;
class QTableWidget;
class QTextEdit;
class QProgressBar;
class MountDialog;
class AppBrowser;
class ImportTable;

class Window : public QMainWindow {
public:
    Window();
private:
    QLineEdit *volume;
    QLineEdit *prefix;
    QLineEdit *darling;
    AppBrowser *available;
    ImportTable *apps;
    QTextEdit *log;
    QProgressBar *progress;
    MountDialog *mountDialog = nullptr;
    QMap<QString, AppEntry> entries;
    int activeProcesses = 0;
    int scanGeneration = 0;
    bool importRunning = false;
    QMap<QString, QJsonArray> chains;
    QMap<QString, QString> outputs;
    QMap<QString, MissingSymbol> pending;
    void refresh();
    void importBundles(const QStringList &names);
    void load();
    void persist();
    void launch(const QString &key);
    void runCommand(const QString &label, const QStringList &args, const QString &key = {});
    QString selectedKey() const;
    void setBusy(bool busy, const QString &message);
};
