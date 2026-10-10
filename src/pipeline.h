// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pipelineworkspace.h"
#include <QFutureWatcher>
#include <QObject>
#include <QProcess>
#include <QTimer>
// Missing-symbol clean-room pipeline: research -> spec -> impl -> validate, one fresh headless agent
// per stage, launcher-copied handoffs that pass HandoffStore, every step in the audit chain.
class MissingSymbolPipeline : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Running, Done, Failed, Cancelled, TimedOut };
    Q_ENUM(State)
    struct Options {
        QStringList agents;            // CLI per stage (research, spec, impl, validate)
        QString trace;                 // T0 trace written by the tracer
        QString symbol, library;       // shown to the agents as the task
        QString supervisor;            // executable understanding --stage-supervisor
        int stageTimeoutSeconds = 30 * 60;
    };
    static constexpr int MaxReplacements = 2, MaxRevisions = 2, MaxValidationRounds = 2, StopGraceSeconds = 10;
    // Default: rotate through the installed CLIs so neighbouring stages use different providers.
    static QStringList defaultAgents(const QStringList &installed);
    MissingSymbolPipeline(PipelineWorkspace workspace, Options options, QObject *parent = nullptr);
    ~MissingSymbolPipeline() override;
    void start();
    void cancel();
    State state() const { return current; }
    QString stage() const { return activeStage; }
    const PipelineWorkspace &workspace() const { return ws; }
signals:
    void statusChanged(const QString &message);
    void finished(MissingSymbolPipeline::State state, const QString &message);
private:
    void launch(const QString &stage);
    void stageExited(int code, QProcess::ExitStatus status);
    void submitted(const HandoffStore::Result &result);
    void handOff(const HandoffStore::Release &release);
    void revise(const QStringList &reasons);
    void taint(const QString &reason);
    void finish(State state, const QString &message);
    bool record(const QString &verdict, QJsonObject fields);
    QString deliver(const HandoffStore::Release &release, const QString &stage, bool asInput);
    QString agentFor(const QString &stage) const;
    QString brief(const QString &stage) const;
    PipelineWorkspace ws;
    Options opts;
    State current = State::Idle;
    QString activeStage, agentId, agentCli, anchor;
    QHash<QString, int> launches, revisions, replacements;
    QHash<QString, QList<HandoffStore::Release>> delivered, inputs; // per stage: all deliveries / those named in Inputs
    int validationRounds = 0, logFd = -1, runLock = -1;
    bool cancelling = false, timedOut = false;
    QProcess *process = nullptr;
    QTimer timeout;
    QFutureWatcher<HandoffStore::Result> watcher;
};
