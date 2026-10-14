// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QObject>
#include <QFutureWatcher>
#include <QUrl>
#include <atomic>

class RuntimeDownload : public QObject {
    Q_OBJECT
public:
    explicit RuntimeDownload(QObject *parent = nullptr, QUrl api = QUrl("https://api.github.com"));
    ~RuntimeDownload() override;
    void start(const QString &dataRoot);
    void cancel();
    bool isRunning() const;
signals:
    void progress(const QString &phase, qint64 received, qint64 total);
    void completed(const QString &runtime, const QString &error);
private:
    struct Result { QString runtime, error; };
    QUrl api;
    std::atomic_bool cancelled{false};
    QFutureWatcher<Result> watcher;
};
