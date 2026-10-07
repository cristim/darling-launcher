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

class Window : public QMainWindow {
public:
    Window();
private:
    QLineEdit *volume;
    QLineEdit *prefix;
    QLineEdit *darling;
    QListWidget *available;
    QTableWidget *apps;
    QTextEdit *log;
    QProgressBar *progress;
    QMap<QString, AppEntry> entries;
    int activeProcesses = 0;
    QMap<QString, QJsonArray> chains;
    QMap<QString, QString> outputs;
    QMap<QString, MissingSymbol> pending;
    void refresh();
    void load();
    void persist();
    void launch(const QString &key);
    void runCommand(const QString &label, const QStringList &args, const QString &key = {});
    QString selectedKey() const;
    void setBusy(bool busy, const QString &message);
};
