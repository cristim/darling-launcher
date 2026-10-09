// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "handoffstore.h"
#include <QHash>
#include <QStringList>
// Per-run directory layout for the missing-symbol pipeline:
//   <base>/<run-id>/{research,spec,impl,validate,handoff}  (0700), <stage>.tainted/<agent-id>/
// While a stage's agent runs, the run root is 0100 (traverse only) and every other directory is 0000.
// All stages run as the same user, so this keeps out honest mistakes and makes tampering detectable
// (any chmod changes ctime); it is not a sandbox.
class PipelineWorkspace {
public:
    static QString defaultBase();
    // New run with a random id under base (created 0700 if missing).
    static PipelineWorkspace create(const QString &base, QString *error);
    // Restores the modes of runs left locked by a crash and logs INTERRUPTED. Returns their roots.
    static QStringList recover(const QString &base);
    // Process ids (other than ours) with a cwd or open file under dir.
    static QList<qint64> processesUsing(const QString &dir);
    QString root, runId;
    bool valid() const { return !root.isEmpty(); }
    QString stageDir(const QString &stage) const { return root + '/' + stage; }
    // Copies a trace into research/trace/T0.jsonl after refusing forbidden fields (caller address,
    // PC, backtrace). The original stays where the tracer wrote it.
    QString importTrace(const QString &path) const;
    // Copies a released handoff into <stage>/inputs/ after re-checking its hash.
    QString deliver(const HandoffStore::Release &release, const QString &stage) const;
    QString lockFor(const QString &stage);
    // Restores 0700 modes; tampered is set when a locked directory changed while locked.
    QString unlock(bool *tampered);
    // Moves the stage workspace to <stage>.tainted/<agentId>/ (never deleted) and recreates it empty.
    QString taint(const QString &stage, const QString &agentId) const;
    // Content that must never be in impl/validate: research files, research releases, raw trace
    // objects, or inputs that were not delivered. Returns the problems found.
    QStringList firewall(const QString &stage, const QStringList &delivered) const;
private:
    struct Locked { quint64 inode; qint64 ctimeSec, ctimeNsec; unsigned mode; };
    QHash<QString, Locked> locked;
};
