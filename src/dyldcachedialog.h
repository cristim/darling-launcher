// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "dyldcachecopy.h"
#include "dyldcachesettings.h"
#include <QDialog>
#include <QFutureWatcher>
#include <QMutex>
#include <atomic>
#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QGroupBox;

// Settings page for the user's private dyld cache store and the per-prefix choice of libraries served from it.
// The store is copied from the selected macOS volume in a background thread; the page never builds a guest
// environment (LaunchEnv does, at launch) and never writes anything but the prefix's dyld-cache.json and policy file.
class DyldCacheDialog : public QDialog {
    Q_OBJECT
public:
    struct App { QString name, executableRel; }; // the app selected in the launcher, if any
    DyldCacheDialog(const QString &volume, const QString &prefix, const QString &storeRoot, const App &app, QWidget *parent = nullptr);
    ~DyldCacheDialog() override;
    bool isBusy() const { return running; }
    void reject() override;
    // Error text for the user; the default is a message box. Tests replace it.
    std::function<void(const QString &title, const QString &text)> showError;
    // Test seam: the copy options (a test shrinks the throttle).
    std::function<DyldCacheCopy::Options()> copyOptions = [] { return DyldCacheCopy::Options{}; };
signals:
    void jobFinished(); // a background job ended and the page has refreshed
private:
    struct JobResult {
        DyldCacheCopy::Outcome outcome;
        QStringList lines;
        QString sourceDir, sourceId, sourceError;
    };
    QString volume, prefix, storeRoot;
    App app;
    bool running = false;
    std::atomic<bool> cancel{false};
    QFutureWatcher<JobResult> watcher;
    std::function<void(const JobResult &)> onDone;
    QMutex progressLock;
    DyldCacheCopy::Progress latest;
    QString latestLabel;
    int latestIndex = 0, latestCount = 1;
    std::atomic<bool> progressPosted{false};
    QString sourceDir, sourceId, sourceError;

    QLabel *sourceLabel, *headline, *rows, *progressLabel, *prefixStatusLabel, *appLabel;
    QLineEdit *location;
    QProgressBar *bar;
    QPushButton *copyButton, *cancelButton, *verifyButton, *refreshButton, *saveButton;
    QGroupBox *prefixGroup;
    QCheckBox *enable;
    QComboBox *storeChoice;
    QList<QCheckBox *> libraries;
    DyldCacheConfig loaded;
    QString loadError; // why the prefix's saved settings could not be read; empty when they were
    bool loadFailed = false, haveSource = false;

    void startJob(const std::function<JobResult()> &work, const std::function<void(const JobResult &)> &done);
    void setRunning(bool busy);
    void probeSource();
    void postProgress(const DyldCacheCopy::Progress &p, const QString &label, int storeIndex = 0, int storeCount = 1);
    void startCopy();
    void startVerify();
    void showOverview();
    void loadPrefix();
    void updatePrefix();
    void savePrefix();
    DyldCacheConfig editedConfig(QString *error) const;
};
