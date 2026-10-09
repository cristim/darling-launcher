// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "handoffcheck.h"
#include <QJsonObject>
#include <QStringList>
// Releases checked handoff files into <run-root>/handoff/ and keeps the append-only, hash-chained
// AUDIT.jsonl. Port of check_handoff.py's run_check/release/verify_chain; the on-disk format is the
// oracle's, so either implementation can verify the other's chain.
namespace HandoffStore {
inline const QStringList Stages{"research", "spec", "impl", "validate"};
constexpr qint64 MaxInput = 1000000;
inline const QString Genesis = QString(64, '0');
bool allowedTransition(const QString &from, const QString &to);
enum class Status { Released, Rejected, Unsafe }; // check_handoff.py exit codes 0, 1, 2
struct Result {
    Status status = Status::Unsafe;
    QList<HandoffCheck::Hit> reasons; // rule text only, never the matched content
    QString released, sha256, error;
    bool reportToCompliance = false;
};
// The file must be a regular, single-link, non-symlink file inside <runRoot>/<from>/.
Result submit(const QString &runRoot, const QString &from, const QString &to, const QString &file, const QString &agentId = {}, const QStringList &inputs = {});
struct Release { QString name, sha256, from, to; };
QList<Release> releases(const QString &runRoot, QString *error);
// Pipeline events (verdict = STAGE, TAINT, CANCEL, TIMEOUT, DONE, FAILED) on the same chain.
QString appendEvent(const QString &runRoot, const QString &verdict, QJsonObject fields);
struct Chain { int exitCode = 2; QStringList problems, warnings; QString summary; };
Chain verifyChain(const QString &runRoot, bool check = true);
}
