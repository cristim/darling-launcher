// SPDX-License-Identifier: GPL-3.0-or-later
#include "pipeline.h"
#include "stagesupervisor.h"
#include "troubleshooting.h"
#include "log.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
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
const QHash<QString, QString> Next{{"research", "spec"}, {"spec", "impl"}, {"impl", "validate"}, {"recon", "impl"}};
bool needsFirewall(const QString &stage) { return stage == "impl" || stage == "validate" || stage == "recon"; }
QStringList names(const QList<HandoffStore::Release> &releases) { QStringList out; for (const auto &r : releases) out << r.name; return out; }
// "records=N last_hash=H": what an outside edit of AUDIT.jsonl would change.
QString chainHead(const QString &root) { return HandoffStore::verifyChain(root, false).summary.section(' ', 0, 1); }
QString describe(const HandoffCheck::Hit &h) { return QString("line %1: [%2] %3").arg(h.line).arg(h.rule, h.description); }
constexpr qint64 ScanChunk = 4 * 1024 * 1024, ScanOverlap = 4096;
const char *Marker = "DARLING_PIPELINE_AGENT";
bool hasMarker(qint64 pid, const QString &agentId) {
    QFile env(QString("/proc/%1/environ").arg(pid));
    return env.open(QIODevice::ReadOnly) && env.readAll().split('\0').contains(QByteArray(Marker) + '=' + agentId.toUtf8());
}
// Compliance rule ids hit by a stage's OUTPUT.md; an output too large to check is a taint reason.
QStringList outputComplianceRules(const QString &path, QStringList *reasons) {
    QFile f(path);
    // Symlinks, FIFOs and other special files are tainted by the caller; never open them (a FIFO would block).
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isFile() || !f.open(QIODevice::ReadOnly)) return {};
    const QByteArray data = f.read(HandoffStore::MaxInput + 1);
    if (data.size() > HandoffStore::MaxInput) { *reasons << "OUTPUT.md is too large to check"; return {}; }
    QStringList rules;
    for (const auto &h : HandoffCheck::checkTextStrict(QString::fromUtf8(data))) if (HandoffCheck::isComplianceRule(h)) rules << (h.inner.isEmpty() ? h.rule : h.inner);
    rules.removeDuplicates(); rules.sort();
    return rules;
}
QString command(qint64 pid) {
    QFile f(QString("/proc/%1/cmdline").arg(pid));
    return f.open(QIODevice::ReadOnly) ? QString::fromLocal8Bit(f.read(200).replace('\0', ' ')).trimmed() : QString("?");
}
// CLI logs in JSON lines escape newlines; scan the decoded strings as well as the raw text.
QString withJsonStrings(const QByteArray &data) {
    QString text = QString::fromUtf8(data);
    for (const QByteArray &line : data.split('\n')) {
        if (!line.trimmed().startsWith('{')) continue;
        QList<QJsonValue> stack{QJsonDocument::fromJson(line).object()};
        while (!stack.isEmpty()) {
            const QJsonValue v = stack.takeLast();
            if (v.isString()) text += '\n' + v.toString();
            else if (v.isObject()) for (const auto &x : v.toObject()) stack << x;
            else if (v.isArray()) for (const auto &x : v.toArray()) stack << x;
        }
    }
    return text;
}
// Exposure markers in a file of any size, read in chunks cut at line ends. Returns a reason or empty.
QString scanFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return " cannot be read for checking";
    auto exposed = [](const QByteArray &part) { return !HandoffCheck::exposureMarkers(withJsonStrings(part)).isEmpty(); };
    QByteArray buffer;
    while (!f.atEnd()) {
        buffer += f.read(ScanChunk);
        qsizetype cut = f.atEnd() ? buffer.size() : buffer.lastIndexOf('\n') + 1;
        if (cut == 0 && buffer.size() < 2 * ScanChunk) continue; // keep reading a long line
        const bool longLine = cut == 0;                           // a line over 8 MB is scanned in pieces
        if (longLine) cut = buffer.size();
        if (exposed(buffer.left(cut))) return " contains disassembly or decompiler markers";
        buffer.remove(0, longLine ? cut - ScanOverlap : cut);  // keep an overlap so no marker is split
    }
    return !buffer.isEmpty() && exposed(buffer) ? " contains disassembly or decompiler markers" : QString();
}
}
bool ReconcileTracker::shouldReconcile(const HandoffStore::Release &release, const QString &text) {
    // `Reconcile: <names>` (SKILL.md), names in the same grammar as the reconciler's `Absent:` line.
    static const QRegularExpression line("(?m)^Reconcile:\\s*[A-Za-z_$][\\w$.@]*(?:\\s*,\\s*[A-Za-z_$][\\w$.@]*)*\\s*$");
    if (release.from != "spec" || release.to != "impl" || !line.match(text).hasMatch() || reconciled.contains(release.sha256)) return false;
    reconciled.insert(release.sha256);
    return true;
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
    if (runLock >= 0) ::close(runLock);
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
    runLock = ws.holdRunLock();
    if (runLock < 0) { finish(State::Failed, "Another launcher process is running this pipeline."); return; }
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
    // Between stages the only asynchronous step is the submit, whose completion handler sees the flag.
    if (process) { process->terminate(); QTimer::singleShot(StopGraceSeconds * 1000, process, &QProcess::kill); }
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
    if (stage == "spec") task += "inputs/ holds the T1 facts. Write a behaviour specification (T2) in terms of public documentation and the guest's observable behaviour. Each requirement cites its rung; a requirement whose only source is a raw-word observation is marked OBSERVED and names a test with an authored, non-Apple fixture. "
        "If the implementation must first know whether some names are already absent from Darling's source, add one line `Reconcile: <name>, <name>`.";
    if (stage == "impl") task += "inputs/ holds only the specification (plus the reconciler's three-line answer and validation mismatch reports, if any). Implement from it, public sources and Darling's source. You have no research material and cannot ask for it. In OUTPUT.md list what you implemented, the files and the tests.";
    if (stage == "recon") task += "inputs/ holds the specification release whose Reconcile: line names symbols. Establish only whether those symbols are absent from Darling's current working tree and whether callers remain. "
        "Read only the working-tree files you need and search for callers by name; never run git log, blame or show and never read commit messages. "
        "OUTPUT.md must be exactly these three lines and nothing else: `Absent: <name>, <name>`, `Branch: <branch>  Commit: <40-hex SHA>`, and `Callers exist: no` or `Callers exist: yes; files: <repo-relative path>, <path>`.";
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
    if (needsFirewall(stage))
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
    env.insert("TMPDIR", dir + "/.tmp"); env.insert(Marker, agentId);
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
    QStringList reasons, strays;
    for (qint64 pid : PipelineWorkspace::processesUsing(ws.root)) {
        // Only processes carrying this agent's marker are its own; a viewer the user opened is not killed.
        if (hasMarker(pid, agentId)) { ::kill(pid_t(pid), SIGKILL); reasons << QString("process %1 of the agent still used the run folder").arg(pid); }
        else strays << QString("%1 (%2)").arg(pid).arg(command(pid));
    }
    bool tampered = false;
    if (const QString error = ws.unlock(&tampered); !error.isEmpty()) {
        // The stage cannot be inspected, so it is treated as tainted and the run stops.
        if (record("TAINT", {{"stage", activeStage}, {"agent", agentId}, {"reason", "cannot unlock the run folder: " + error}, {"report_to_compliance", true}}))
            finish(State::Failed, error);
        return;
    }
    if (tampered) reasons << "a folder locked during the stage was changed";
    const QString dir = ws.stageDir(activeStage);
    if (QFileInfo::exists(dir + "/TAINTED.md")) reasons << "the agent reported exposure in TAINTED.md";
    for (QDirIterator it(dir, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories); it.hasNext();) {
        const QString path = it.next(), rel = path.mid(dir.size() + 1);
        // OUTPUT.md gets the full check with rule-level compliance reporting below.
        if (rel.startsWith("inputs/") || rel.startsWith("trace/") || rel.startsWith(".tmp/") || rel == "OUTPUT.md") continue;
        if (const QString problem = scanFile(path); !problem.isEmpty()) reasons << rel + problem;
    }
    if (needsFirewall(activeStage)) reasons += ws.firewall(activeStage, names(delivered.value(activeStage)));
    // OUTPUT.md is checked here on every path (cancel, timeout, stray, bad verdict), not only when submitted.
    const QStringList rules = outputComplianceRules(dir + "/OUTPUT.md", &reasons);
    // On the normal path the store rejects it and submitted() reports it with the REJECT record; on every
    // other path (cancel, timeout, stray, other taint reasons, validation reports) it is reported here.
    const bool storeWillReport = reasons.isEmpty() && strays.isEmpty() && !cancelling && !timedOut && activeStage != "validate";
    if (!rules.isEmpty() && !storeWillReport) reasons.prepend(reportCompliance(rules));
    // An unmarked process may be the agent's (the marker is easy to drop), so the run never continues past it.
    if (!strays.isEmpty()) {
        if (!reasons.isEmpty() && !discard(reasons.join("; "), rules)) return;
        if (record("STRAY", {{"stage", activeStage}, {"processes", QJsonArray::fromStringList(strays)}}))
            finish(State::Failed, "These processes still use the pipeline folder: " + strays.join(", ") + ". Close them and start the pipeline again.");
        return;
    }
    // A stage that saw disallowed material is tainted even when it was cancelled or timed out.
    if (!reasons.isEmpty() && (cancelling || timedOut) && !discard(reasons.join("; "), rules)) return;
    if (cancelling) { if (record("CANCEL", {{"stage", activeStage}, {"agent", agentId}})) finish(State::Cancelled, "Cancelled."); return; }
    if (timedOut) { if (record("TIMEOUT", {{"stage", activeStage}, {"agent", agentId}})) finish(State::TimedOut, "The " + activeStage + " stage timed out."); return; }
    if (!reasons.isEmpty()) { taint(reasons.join("; "), rules); return; }
    const QString output = dir + "/OUTPUT.md";
    struct stat st {};
    if (::lstat(QFile::encodeName(output).constData(), &st) != 0) { revise({QString("no OUTPUT.md was written (agent exit status %1)").arg(code)}); return; }
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1) { taint("OUTPUT.md is a symlink, hard link or special file"); return; }
    QString to = Next.value(activeStage);
    if (activeStage == "validate") {
        QFile f(output); f.open(QIODevice::ReadOnly);
        const QString body = QString::fromUtf8(f.read(HandoffStore::MaxInput)), verdict = body.section('\n', 0, 0).trimmed();
        if (verdict == "VERDICT: PASS") {
            QStringList problems;
            for (const auto &h : HandoffCheck::checkTextStrict(body)) problems << describe(h);
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
    emit statusChanged("Checking the " + activeStage + " output before it is handed on…");
    const auto submit = opts.submit ? opts.submit : HandoffStore::submit;
    // Any exception in the check is a failed check, never a pass.
    watcher.setFuture(QtConcurrent::run([submit, root = ws.root, from = activeStage, to, output, agent = agentId, cited = names(inputs.value(activeStage))] {
        try { return submit(root, from, to, output, agent, cited); }
        catch (const std::exception &e) { HandoffStore::Result r; r.error = QString("internal error: ") + e.what(); return r; }
        catch (...) { HandoffStore::Result r; r.error = "internal error"; return r; }
    }));
}
bool MissingSymbolPipeline::discard(const QString &reason, const QStringList &complianceRules) {
    if (!record("TAINT", {{"stage", activeStage}, {"agent", agentId}, {"reason", reason}, {"moved_to", activeStage + ".tainted/" + agentId}, {"report_to_compliance", true}, {"compliance_rules", QJsonArray::fromStringList(complianceRules)}})) return false;
    if (const QString error = ws.taint(activeStage, agentId); !error.isEmpty()) { finish(State::Failed, error); return false; }
    return true;
}
QString MissingSymbolPipeline::reportCompliance(const QStringList &rules) {
    const QString report = "REPORT-TO-COMPLIANCE: rule " + rules.join(", rule ");
    LauncherLog::write("pipeline", ws.runId + " " + activeStage + ": " + report);
    emit statusChanged(report + " (" + activeStage + " stage)");
    emit complianceReport(activeStage, rules);
    return report;
}
void MissingSymbolPipeline::submitted(const HandoffStore::Result &result) {
    anchor = chainHead(ws.root);
    if (current != State::Running) return;
    QStringList reasons; for (const auto &h : result.reasons) reasons << describe(h);
    // A compliance hit is reported before anything else can end the run: cancel, store failure, taint limit.
    const QString report = result.reportToCompliance ? reportCompliance(result.complianceRules) : QString();
    if (result.status == HandoffStore::Status::Unsafe) { finish(State::Failed, "Cannot check the " + activeStage + " output: " + result.error); return; }
    if (!report.isEmpty() && cancelling && !discard(report, result.complianceRules)) return;
    if (cancelling) { if (record("CANCEL", {{"stage", activeStage}})) finish(State::Cancelled, "Cancelled."); return; }
    if (result.status == HandoffStore::Status::Rejected) {
        if (!report.isEmpty()) taint(report + " (" + reasons.join("; ") + ")", result.complianceRules);
        else revise(reasons);
        return;
    }
    QString error;
    for (const auto &r : HandoffStore::releases(ws.root, &error)) if (r.name == result.released) { handOff(r); return; }
    finish(State::Failed, "The released handoff is missing from the audit log. " + error);
}
QString MissingSymbolPipeline::releasedText(const HandoffStore::Release &release) const {
    QFile f(ws.stageDir(release.to) + "/inputs/" + release.name);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.read(HandoffStore::MaxInput)) : QString();
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
    // A specification may ask the reconciler whether names are already absent from Darling's tree; recon sees
    // only that release and the working tree, and its answer goes to impl only.
    QString stage = next;
    if (error.isEmpty() && reconciler.shouldReconcile(release, releasedText(release))) {
        stage = "recon";
        error = deliver(release, "recon", true);
        if (error.isEmpty()) { delivered["recon"] << release; inputs["recon"] << release; }
    }
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
    revisions[stage] = 0;
    launch(stage);
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
void MissingSymbolPipeline::taint(const QString &reason, const QStringList &complianceRules) {
    const QString stage = activeStage;
    if (!discard(reason, complianceRules)) return;
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
    if (logFd >= 0) { ::close(logFd); logFd = -1; }
    bool tampered = false; ws.unlock(&tampered); // no-op unless a launch failed after locking
    if (state == State::Failed) HandoffStore::appendEvent(ws.root, "FAILED", {{"stage", activeStage}, {"reason", message}});
    emit statusChanged(message);
    emit finished(state, message);
}
