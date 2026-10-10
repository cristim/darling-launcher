// SPDX-License-Identifier: GPL-3.0-or-later
#include "pipeline.h"
#include "stagesupervisor.h"
#include "troubleshooting.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <QtConcurrent>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
namespace {
const QHash<QString, QString> Next{{"research", "spec"}, {"spec", "impl"}, {"impl", "validate"}};
QStringList names(const QList<HandoffStore::Release> &releases) { QStringList out; for (const auto &r : releases) out << r.name; return out; }
// "records=N last_hash=H": what an outside edit of AUDIT.jsonl would change.
QString chainHead(const QString &root) { return HandoffStore::verifyChain(root, false).summary.section(' ', 0, 1); }
QString describe(const HandoffCheck::Hit &h) { return QString("line %1: [%2] %3").arg(h.line).arg(h.rule, h.description); }
}
QStringList MissingSymbolPipeline::defaultAgents(const QStringList &installed) {
    QStringList out;
    for (int i = 0; i < HandoffStore::Stages.size() && !installed.isEmpty(); ++i) out << installed[i % installed.size()];
    return out;
}
MissingSymbolPipeline::MissingSymbolPipeline(PipelineWorkspace workspace, Options options, QObject *parent) : QObject(parent), ws(std::move(workspace)), opts(std::move(options)) {
    if (opts.supervisor.isEmpty()) opts.supervisor = QCoreApplication::applicationFilePath();
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, this, [this] {
        timedOut = true;
        if (process) { process->terminate(); QTimer::singleShot(StopGraceSeconds * 1000, process, &QProcess::kill); }
    });
    connect(&watcher, &QFutureWatcher<HandoffStore::Result>::finished, this, [this] { submitted(watcher.result()); });
}
MissingSymbolPipeline::~MissingSymbolPipeline() {
    if (!process) return;
    process->disconnect();
    process->terminate();
    if (!process->waitForFinished(StopGraceSeconds * 1000)) process->kill();
    process->waitForFinished(1000);
    bool tampered = false; ws.unlock(&tampered);
}
bool MissingSymbolPipeline::record(const QString &verdict, QJsonObject fields) {
    if (!anchor.isEmpty() && chainHead(ws.root) != anchor) { finish(State::Failed, "The audit log changed outside the launcher; the run is stopped."); return false; }
    if (const QString error = HandoffStore::appendEvent(ws.root, verdict, fields); !error.isEmpty()) { finish(State::Failed, "Cannot write the audit log: " + error); return false; }
    anchor = chainHead(ws.root);
    return true;
}
void MissingSymbolPipeline::start() {
    if (current != State::Idle) return;
    current = State::Running;
    if (opts.agents.size() != HandoffStore::Stages.size()) { finish(State::Failed, "Choose an agent for each stage."); return; }
    if (!record("START", {{"run", ws.runId}, {"symbol", opts.symbol}, {"library", opts.library}, {"agents", QJsonArray::fromStringList(opts.agents)}})) return;
    if (const QString error = ws.importTrace(opts.trace); !error.isEmpty()) {
        record("TRACE_REFUSED", {{"reason", error}, {"report_to_compliance", error.contains("non-compliant")}});
        finish(State::Failed, error); return;
    }
    launch("research");
}
void MissingSymbolPipeline::cancel() {
    if (current != State::Running) return;
    cancelling = true;
    if (process) { process->terminate(); QTimer::singleShot(StopGraceSeconds * 1000, process, &QProcess::kill); }
    else if (!watcher.isRunning() && record("CANCEL", {{"stage", activeStage}})) finish(State::Cancelled, "Cancelled.");
}
QString MissingSymbolPipeline::agentFor(const QString &stage) const {
    const QString chosen = opts.agents.value(HandoffStore::Stages.indexOf(stage));
    const QStringList installed = LauncherTroubleshooting::agents();
    // A replacement for a tainted agent prefers another provider when one is installed.
    const int base = qMax(0, int(installed.indexOf(chosen)));
    return replacements.value(stage) && !installed.isEmpty() ? installed[(base + replacements.value(stage)) % installed.size()] : chosen;
}
QString MissingSymbolPipeline::brief(const QString &stage) const {
    const QString dir = ws.stageDir(stage);
    QString task = "You are the " + stage + " agent of a clean-room pipeline for the unresolved import " + opts.symbol + (opts.library.isEmpty() ? QString() : " from " + opts.library)
        + ". Darling's AGENTS.md section 1 applies. Work only in " + dir + "; other stages' folders are locked and touching them discards your work. "
          "Never disassemble, decompile, class-dump or read machine code of any Apple binary, and never run otool, objdump, strings, nm (beyond nm -u), gdb or lldb on one. "
          "If you see disassembly, decompiled text or a class dump, even by accident, stop and write TAINTED.md saying what you saw. "
          "Write your result to OUTPUT.md in that folder. Do not add Agent: or Inputs: header lines (the launcher records them); cite sources on one line starting with `Sources:`, separated by `;`. "
          "If REJECTED.txt exists, your previous OUTPUT.md was rejected for the listed rules: rewrite it without that content. ";
    if (stage == "research") task += "trace/T0.jsonl is the raw trace (T0) of unresolved imports. Write T1 facts: per symbol its name, framework, call count and position among first calls. "
        "Arguments only as (a) a type or meaning that follows from Apple's open source, vendored headers or public documentation, cited, or (b) 'raw word W observed N times in position k' labelled rung 4 with the guest app and version. "
        "No hypotheses about Apple's internals, never copy trace lines, at most 4 raw words in total.";
    if (stage == "spec") task += "inputs/ holds the T1 facts. Write a behaviour specification (T2) in terms of public documentation and the guest's observable behaviour. Each requirement cites its rung; a requirement whose only source is a raw-word observation is marked OBSERVED and names a test with an authored, non-Apple fixture.";
    if (stage == "impl") task += "inputs/ holds only the specification (and validation mismatch reports, if any). Implement from it, public sources and Darling's source. You have no research material and cannot ask for it. In OUTPUT.md list what you implemented, the files and the tests.";
    if (stage == "validate") task += "inputs/ holds the specification and the implementation report. Compare the guest-visible behaviour with the specification. The first line of OUTPUT.md is `VERDICT: PASS` or `VERDICT: MISMATCH`, followed by mismatches described as behaviours, never as code.";
    return task;
}
void MissingSymbolPipeline::launch(const QString &stage) {
    if (current != State::Running) return;
    activeStage = stage;
    agentCli = agentFor(stage);
    agentId = QString("%1-%2-%3-%4").arg(stage, agentCli).arg(++launches[stage]).arg(QUuid::createUuid().toString(QUuid::Id128).left(8));
    const QString dir = ws.stageDir(stage), executable = QStandardPaths::findExecutable(agentCli);
    if (executable.isEmpty()) { finish(State::Failed, agentCli + " is no longer installed."); return; }
    if (stage == "impl" || stage == "validate")
        if (const QStringList problems = ws.firewall(stage, names(delivered.value(stage))); !problems.isEmpty()) {
            record("FIREWALL", {{"stage", stage}, {"problems", QJsonArray::fromStringList(problems)}, {"report_to_compliance", true}});
            finish(State::Failed, "The " + stage + " workspace holds material it must never see: " + problems.join("; ")); return;
        }
    QSaveFile briefFile(dir + "/BRIEF.md");
    if (!briefFile.open(QIODevice::WriteOnly) || briefFile.write(brief(stage).toUtf8()) < 0 || !briefFile.commit()) { finish(State::Failed, "Cannot write the " + stage + " brief."); return; }
    QDir().mkpath(dir + "/.tmp");
    if (!record("STAGE", {{"stage", stage}, {"agent", agentId}, {"cli", agentCli}, {"launch", launches[stage]}})) return;
    // Opened once: the agent can rename or replace AGENT.log but not redirect the launcher's writes.
    logFd = ::open(QFile::encodeName(dir + "/AGENT.log").constData(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (logFd < 0) { finish(State::Failed, "Cannot open the " + stage + " agent log."); return; }
    if (const QString error = ws.lockFor(stage); !error.isEmpty()) { finish(State::Failed, error); return; }
    QStringList args = LauncherTroubleshooting::backgroundArguments(agentCli, brief(stage), {{dir}, {}});
    if (agentCli == "claude") args.prepend("--no-session-persistence");
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const QString &name : env.keys()) if (name.startsWith("DYLD_") || name.startsWith("DARLING_MISSING_SYMBOLS")) env.remove(name);
    env.insert("TMPDIR", dir + "/.tmp");
    timedOut = false;
    process = new QProcess(this);
    process->setWorkingDirectory(dir); process->setProcessChannelMode(QProcess::MergedChannels); process->setProcessEnvironment(env);
    connect(process, &QProcess::readyReadStandardOutput, this, [this] { const QByteArray b = process->readAllStandardOutput(); if (logFd >= 0 && ::write(logFd, b.constData(), size_t(b.size())) < 0) {} });
    connect(process, &QProcess::finished, this, &MissingSymbolPipeline::stageExited);
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) { if (error == QProcess::FailedToStart) stageExited(127, QProcess::CrashExit); });
    process->start(opts.supervisor, QStringList{StageSupervisor::Flag, executable} + args);
    timeout.start(opts.stageTimeoutSeconds * 1000);
    emit statusChanged(agentCli + " is working on the " + stage + " stage. Private log: " + dir + "/AGENT.log");
}
void MissingSymbolPipeline::stageExited(int code, QProcess::ExitStatus) {
    if (!process) return;
    timeout.stop();
    const QByteArray rest = process->readAllStandardOutput();
    if (logFd >= 0) { if (::write(logFd, rest.constData(), size_t(rest.size())) < 0) {} ::close(logFd); logFd = -1; }
    process->disconnect(this); process->deleteLater(); process = nullptr;
    QStringList reasons;
    if (const auto pids = PipelineWorkspace::processesUsing(ws.root); !pids.isEmpty()) {
        for (qint64 pid : pids) ::kill(pid_t(pid), SIGKILL);
        reasons << QString("%1 process(es) still used the run folder after the agent exited").arg(pids.size());
    }
    bool tampered = false;
    if (const QString error = ws.unlock(&tampered); !error.isEmpty()) { finish(State::Failed, error); return; }
    if (tampered) reasons << "a folder locked during the stage was changed";
    if (cancelling) { if (record("CANCEL", {{"stage", activeStage}, {"agent", agentId}})) finish(State::Cancelled, "Cancelled."); return; }
    if (timedOut) { if (record("TIMEOUT", {{"stage", activeStage}, {"agent", agentId}})) finish(State::TimedOut, "The " + activeStage + " stage timed out."); return; }
    const QString dir = ws.stageDir(activeStage);
    if (QFileInfo::exists(dir + "/TAINTED.md")) reasons << "the agent reported exposure in TAINTED.md";
    static const QRegularExpression scanned("(?:\\.md|\\.txt|\\.log|\\.json)$");
    for (QDirIterator it(dir, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories); it.hasNext();) {
        const QString path = it.next(), rel = path.mid(dir.size() + 1);
        if (rel.startsWith("inputs/") || rel.startsWith("trace/") || !scanned.match(rel).hasMatch()) continue;
        QFile f(path);
        if (f.open(QIODevice::ReadOnly) && !HandoffCheck::exposureMarkers(QString::fromUtf8(f.read(8 * 1024 * 1024))).isEmpty()) reasons << rel + " contains disassembly or decompiler markers";
    }
    if (activeStage == "impl" || activeStage == "validate") reasons += ws.firewall(activeStage, names(delivered.value(activeStage)));
    if (!reasons.isEmpty()) { taint(reasons.join("; ")); return; }
    const QString output = dir + "/OUTPUT.md";
    struct stat st {};
    if (::lstat(QFile::encodeName(output).constData(), &st) != 0) { revise({QString("no OUTPUT.md was written (agent exit status %1)").arg(code)}); return; }
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1) { taint("OUTPUT.md is a symlink, hard link or special file"); return; }
    QString to = Next.value(activeStage);
    if (activeStage == "validate") {
        QFile f(output); f.open(QIODevice::ReadOnly);
        const QString body = QString::fromUtf8(f.read(HandoffStore::MaxInput)), verdict = body.section('\n', 0, 0).trimmed();
        if (verdict == "VERDICT: PASS") {
            QStringList problems; bool exposure = false;
            for (const auto &h : HandoffCheck::checkTextStrict(body)) { problems << describe(h); exposure = exposure || HandoffCheck::isComplianceRule(h); }
            if (exposure) { taint("the validation report matched compliance rules"); return; }
            if (!problems.isEmpty()) { revise(problems); return; }
            const auto chain = HandoffStore::verifyChain(ws.root);
            if (chain.exitCode != 0) { finish(State::Failed, "The audit chain does not verify: " + chain.problems.join("; ")); return; }
            if (record("DONE", {{"stage", "validate"}, {"agent", agentId}, {"report_sha256", QString::fromLatin1(QCryptographicHash::hash(body.toUtf8(), QCryptographicHash::Sha256).toHex())}}))
                finish(State::Done, "Validation passed. Audit: " + HandoffStore::verifyChain(ws.root).summary);
            return;
        }
        if (verdict != "VERDICT: MISMATCH") { revise({"the first line of OUTPUT.md must be VERDICT: PASS or VERDICT: MISMATCH"}); return; }
        if (++validationRounds > MaxValidationRounds) { finish(State::Failed, "The implementation still mismatches the specification after " + QString::number(MaxValidationRounds) + " rounds."); return; }
        to = "impl";
    }
    if (!anchor.isEmpty() && chainHead(ws.root) != anchor) { finish(State::Failed, "The audit log changed outside the launcher; the run is stopped."); return; }
    watcher.setFuture(QtConcurrent::run(HandoffStore::submit, ws.root, activeStage, to, output, agentId, names(inputs.value(activeStage))));
}
void MissingSymbolPipeline::submitted(const HandoffStore::Result &result) {
    anchor = chainHead(ws.root);
    if (current != State::Running) return;
    if (cancelling) { if (record("CANCEL", {{"stage", activeStage}})) finish(State::Cancelled, "Cancelled."); return; }
    if (result.status == HandoffStore::Status::Unsafe) { finish(State::Failed, "Cannot check the " + activeStage + " output: " + result.error); return; }
    if (result.status == HandoffStore::Status::Rejected) {
        QStringList reasons; for (const auto &h : result.reasons) reasons << describe(h);
        if (result.reportToCompliance) taint("the handoff matched compliance rules: " + reasons.join("; "));
        else revise(reasons);
        return;
    }
    QString error;
    for (const auto &r : HandoffStore::releases(ws.root, &error)) if (r.name == result.released) { handOff(r); return; }
    finish(State::Failed, "The released handoff is missing from the audit log. " + error);
}
QString MissingSymbolPipeline::deliver(const HandoffStore::Release &release, const QString &stage, bool asInput) {
    if (const QString error = ws.deliver(release, stage); !error.isEmpty()) return error;
    if (!record("DELIVER", {{"stage", stage}, {"input", release.name}, {"input_sha256", release.sha256}, {"listed_in_inputs", asInput}})) return "audit";
    return {};
}
void MissingSymbolPipeline::handOff(const HandoffStore::Release &release) {
    const QString next = release.to;
    QString error = deliver(release, next, true);
    if (error.isEmpty()) { delivered[next] << release; inputs[next] << release; }
    // Validation also needs the specification, which was released to impl: delivered, not cited in Inputs.
    if (error.isEmpty() && next == "validate") {
        QString listError; HandoffStore::Release spec;
        for (const auto &r : HandoffStore::releases(ws.root, &listError)) if (r.from == "spec" && r.to == "impl") spec = r;
        const auto toValidate = delivered.value("validate");
        const bool already = std::any_of(toValidate.begin(), toValidate.end(), [&](const auto &d) { return d.name == spec.name; });
        if (spec.name.isEmpty()) error = "no specification was released";
        else if (!already) { error = deliver(spec, "validate", false); if (error.isEmpty()) delivered["validate"] << spec; }
    }
    if (current != State::Running) return;
    if (!error.isEmpty()) { finish(State::Failed, "Cannot deliver the handoff: " + error); return; }
    revisions[next] = 0;
    launch(next);
}
void MissingSymbolPipeline::revise(const QStringList &reasons) {
    if (++revisions[activeStage] > MaxRevisions) {
        finish(State::Failed, "The " + activeStage + " output was rejected " + QString::number(MaxRevisions + 1) + " times: " + reasons.join("; "));
        return;
    }
    QSaveFile f(ws.stageDir(activeStage) + "/REJECTED.txt");
    if (!f.open(QIODevice::WriteOnly) || f.write((reasons.join('\n') + '\n').toUtf8()) < 0 || !f.commit()) { finish(State::Failed, "Cannot write REJECTED.txt."); return; }
    if (record("REVISE", {{"stage", activeStage}, {"agent", agentId}, {"reasons", QJsonArray::fromStringList(reasons)}})) launch(activeStage);
}
void MissingSymbolPipeline::taint(const QString &reason) {
    const QString stage = activeStage;
    if (!record("TAINT", {{"stage", stage}, {"agent", agentId}, {"reason", reason}, {"moved_to", stage + ".tainted/" + agentId}, {"report_to_compliance", true}})) return;
    if (const QString error = ws.taint(stage, agentId); !error.isEmpty()) { finish(State::Failed, error); return; }
    emit statusChanged("The " + stage + " agent was tainted (" + reason + "). Its work is discarded; a fresh agent takes over.");
    if (++replacements[stage] > MaxReplacements) {
        finish(State::Failed, "Too many tainted " + stage + " agents; the last reason: " + reason);
        return;
    }
    revisions[stage] = 0;
    QString error = stage == "research" ? ws.importTrace(opts.trace) : QString();
    for (const auto &r : delivered.value(stage)) if (error.isEmpty()) error = deliver(r, stage, inputs.value(stage).contains(r));
    if (current != State::Running) return;
    if (!error.isEmpty()) { finish(State::Failed, "Cannot prepare a fresh " + stage + " workspace: " + error); return; }
    launch(stage);
}
void MissingSymbolPipeline::finish(State state, const QString &message) {
    if (current != State::Running) return;
    current = state;
    timeout.stop();
    if (state == State::Failed) HandoffStore::appendEvent(ws.root, "FAILED", {{"stage", activeStage}, {"reason", message}});
    emit statusChanged(message);
    emit finished(state, message);
}
