// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pipelineworkspace.h"
#include <QFutureWatcher>
#include <functional>
#include <QObject>
#include <QSet>
#include <QProcess>
#include <QTimer>
// Missing-symbol clean-room pipeline: research -> spec -> impl -> validate, one fresh headless agent
// per stage, launcher-copied handoffs that pass HandoffStore, every step in the audit chain.
// Recon runs once per spec release that carries a `Reconcile:` line (compliance ruling): a revised
// spec with its own line gets a new reconciler, the same release never a second one.
class ReconcileTracker {
public:
    bool shouldReconcile(const HandoffStore::Release &release, const QString &text);
private:
    QSet<QString> reconciled; // sha256 of spec->impl releases already reconciled
};
// Instruction and config files the agent CLIs load by themselves (ancestor CLAUDE.md/AGENTS.md, user
// settings). They are hashed when a run starts; a change before a later stage stops the run.
class AutoLoadedFiles {
public:
    static AutoLoadedFiles snapshot(const QString &runRoot);
    QString changed() const; // first changed path, or empty
private:
    QHash<QString, QByteArray> hashes; // path -> sha256, empty when absent
};
class MissingSymbolPipeline : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Running, Done, Failed, Cancelled, TimedOut };
    Q_ENUM(State)
    struct Options {
        QStringList agents;            // CLI per stage, in HandoffStore::Stages order (recon last)
        QString trace;                 // T0 trace written by the tracer
        QString symbol, library;       // shown to the agents as the task
        QString supervisor;            // executable understanding --stage-supervisor
        int stageTimeoutSeconds = 30 * 60;
        // Tests inject store failures and exceptions here; empty means HandoffStore::submit.
        std::function<HandoffStore::Result(const QString &, const QString &, const QString &, const QString &, const QString &, const QStringList &)> submit;
        std::function<QString(const QString &, const QString &, QJsonObject)> appendEvent;  // default HandoffStore::appendEvent
        std::function<bool(const QString &, const QString &)> complianceLog;               // default LauncherLog::writeChecked
    };
    static constexpr int MaxReplacements = 2, MaxRevisions = 2, MaxValidationRounds = 2, StopGraceSeconds = 10;
    // Default: rotate through the installed CLIs so neighbouring stages use different providers.
    static QStringList defaultAgents(const QStringList &installed);
    // Empty when the guest-supplied names are safe to put in a brief.
    static QString checkNames(const QString &symbol, const QString &library);
    // The environment a stage agent of this CLI gets: an allow-list of the user's environment.
    static QProcessEnvironment stageEnvironment(const QProcessEnvironment &from, const QString &cli);
    // Command line of a stage agent: no user hooks, MCP servers, instruction files or shell where the CLI allows it.
    static QStringList stageArguments(const QString &cli, const QString &dir);
    static constexpr const char *OpencodePermissions = R"({"permission":{"bash":"deny","webfetch":"deny","external_directory":"deny"}})";
    MissingSymbolPipeline(PipelineWorkspace workspace, Options options, QObject *parent = nullptr);
    ~MissingSymbolPipeline() override;
    void start();
    void cancel();
    State state() const { return current; }
    QString stage() const { return activeStage; }
    const PipelineWorkspace &workspace() const { return ws; }
    QString closedRoot; // where the run folder went when the run ended
signals:
    void statusChanged(const QString &message);
    void finished(MissingSymbolPipeline::State state, const QString &message);
    // A handoff matched R1/R3/R5/R10: coord-compliance must be told (the stage is also tainted).
    void complianceReport(const QString &stage, const QStringList &rules);
private:
    void launch(const QString &stage);
    void stageExited(int code, QProcess::ExitStatus status);
    void submitted(const HandoffStore::Result &result);
    void handOff(const HandoffStore::Release &release);
    void revise(const QStringList &reasons);
    void taint(const QString &reason, const QStringList &complianceRules = {});
    // Audit record, log line, status and signal, before any other state change; false = run aborted (stage moved aside).
    bool reportCompliance(const QStringList &rules, QString *report);
    bool discard(const QString &reason, const QStringList &complianceRules); // TAINT record + move aside; false if the run ended
    QString releasedText(const HandoffStore::Release &release) const;
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
    bool cancelling = false, timedOut = false, keepLocked = false;
    ReconcileTracker reconciler;
    AutoLoadedFiles autoLoaded;
    QStringList submittedRules; // compliance rules found in the output being submitted
    QProcess *process = nullptr;
    QTimer timeout;
    QFutureWatcher<HandoffStore::Result> watcher;
};
