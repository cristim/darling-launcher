// SPDX-License-Identifier: GPL-3.0-or-later
#include "troubleshooting.h"
#include "log.h"
#include <QRandomGenerator>
#include <QMessageAuthenticationCode>
#include <QCryptographicHash>
#include "core.h"
#include "discovery.h"
#include "prefixbuilder.h"
#include <QComboBox>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimer>
#include <QTextEdit>
#include <QTextDocument>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QPointer>
#include <QGroupBox>
#include <QMainWindow>
#include <QStatusBar>
#include <QSettings>
#include <QDialogButtonBox>
#include <QtConcurrent>
namespace {
class IssueApprovalDialog : public QDialog {
public:
    using QDialog::QDialog;
    ~IssueApprovalDialog() override { for (auto *process : findChildren<QProcess *>()) process->disconnect(); }
    void done(int result) override {
        for (auto *process : findChildren<QProcess *>()) if (process->state() != QProcess::NotRunning) return;
        QDialog::done(result);
    }
};
QString query(const QString &program, const QStringList &args, bool *ok = nullptr) {
    QProcess process;
    if (QFileInfo(program).fileName() == "git") {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        const QStringList safe{"core.fsmonitor", "false", "core.hooksPath", "/dev/null", "diff.external", "", "core.pager", "cat", "core.sshCommand", "false"};
        env.insert("GIT_CONFIG_COUNT", QString::number(safe.size() / 2)); env.remove("GIT_EXTERNAL_DIFF");
        for (int i = 0; i < safe.size() / 2; ++i) { env.insert("GIT_CONFIG_KEY_" + QString::number(i), safe[2 * i]); env.insert("GIT_CONFIG_VALUE_" + QString::number(i), safe[2 * i + 1]); }
        process.setProcessEnvironment(env);
    }
    process.start(program, args);
    const bool success = process.waitForFinished(3000) && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    if (ok) *ok = success;
    return success ? QString::fromUtf8(process.readAllStandardOutput()).trimmed() : QString();
}
}
QStringList LauncherTroubleshooting::agents() {
    QStringList result;
    for (const QString &name : {"claude", "codex", "opencode"}) if (!QStandardPaths::findExecutable(name).isEmpty()) result << name;
    return result;
}
QStringList LauncherTroubleshooting::agentArguments(const QString &agent, const QString &prompt) {
    if (agent == "opencode") return {"--prompt", prompt};
    if (agent == "codex") return {"--model", "gpt-6.1-sol", "--config", "model_reasoning_effort=\"low\"", prompt};
    if (agent == "claude") return {prompt};
    return {};
}
QStringList LauncherTroubleshooting::backgroundArguments(const QString &agent, const QString &prompt, const AgentAccess &access) {
    if (agent == "codex") return {"exec", "--sandbox", "workspace-write", "--skip-git-repo-check", "--model", "gpt-6.1-sol", "--config", "model_reasoning_effort=\"low\"", "--json", prompt};
    if (agent == "claude") {
        if (access.directories.isEmpty()) return {"--print", "--permission-mode", "acceptEdits", prompt};
        QStringList tools{"Read", "Edit", "Write", "Glob", "Grep"}; tools += access.tools;
        QStringList result{"--print", "--permission-mode", "acceptEdits", "--allowedTools", tools.join(',')};
        for (const QString &dir : access.directories) if (!dir.isEmpty()) result << "--add-dir" << dir;
        return result << "--" << prompt;
    }
    if (agent == "opencode") return {"run", "--format", "json", prompt};
    return {};
}
FixVerifier::FixVerifier(const QString &workspace, const QJsonObject &diagnostic, int stableSeconds, QObject *parent) : QObject(parent), workspace(workspace), diagnostic(diagnostic), stableSeconds(stableSeconds) {
    process.setProcessChannelMode(QProcess::MergedChannels);
}
void FixVerifier::finish(bool verified, const QString &reason) {
    running = false;
    QJsonObject report{{"verified", verified}, {"reason", reason}, {"launcher", launcher}, {"runtimeRoot", runtimeRoot}, {"bundle", diagnostic.value("bundle")}, {"prefix", diagnostic.value("prefix")}, {"proposalCommit", proposalCommit}, {"stableSeconds", stableSeconds}, {"outputTail", output.right(4000)}, {"mac", LauncherTroubleshooting::verificationMac(workspace, proposalCommit, verified)}};
    for (const QString &path : {LauncherTroubleshooting::verificationFile(workspace, proposalCommit), workspace + "/LAUNCHER-VERIFICATION.json"}) {
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) continue;
        QSaveFile file(path);
        if (file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(report).toJson()) >= 0) file.commit();
    }
    LauncherLog::write("verify", (verified ? "verified: " : "not verified: ") + reason);
    emit finished({verified, reason});
}
void FixVerifier::start() {
    running = true;
    launcher = diagnostic.value("launcher").toString(); runtimeRoot = diagnostic.value("runtime").toString();
    if (!QFileInfo(launcher).isExecutable()) { finish(false, "No usable launcher is configured to run the app in the prefix."); return; }
    QFile proposal(workspace + "/proposal.json"); QJsonObject fields;
    if (proposal.open(QIODevice::ReadOnly)) fields = QJsonDocument::fromJson(proposal.readAll()).object();
    bool ok = false;
    const QString source = LauncherTroubleshooting::trustedWorktree(workspace, fields.value("source").toString());
    if (source.isEmpty()) { finish(false, "proposal.json must name the agent's worktree of the cloned source inside the workspace."); return; }
    proposalCommit = query(QStandardPaths::findExecutable("git"), {"-C", source, "rev-parse", "HEAD"}, &ok);
    if (!ok) { finish(false, "The agent produced no committed proposal.json patch to verify."); return; }
    const QString prefix = diagnostic.value("prefix").toString(), bundle = diagnostic.value("bundle").toString(), executable = diagnostic.value("executable").toString();
    if (!QFileInfo(prefix).isDir() || bundle.isEmpty() || executable.isEmpty()) { finish(false, "The failed app or its prefix is no longer available."); return; }
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const auto &name : env.keys()) if (name.startsWith("DYLD_")) env.remove(name);
    env.insert("DPREFIX", prefix); env.insert("DARLING_INSTALL_PREFIX", runtimeRoot);
    process.setProcessEnvironment(env);
    connect(&process, &QProcess::readyReadStandardOutput, this, [this] { output += QString::fromLocal8Bit(process.readAllStandardOutput()); });
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) { if (error == QProcess::FailedToStart && running) finish(false, "Cannot start the patched launcher: " + process.errorString()); });
    connect(&process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus exit) {
        if (!running) return;
        output += QString::fromLocal8Bit(process.readAllStandardOutput());
        const auto missing = LauncherCore::diagnose(output);
        if (exit == QProcess::NormalExit && code == 0 && !missing.valid()) finish(true, "The app ran and exited cleanly in the prefix.");
        else finish(false, missing.valid() ? "The app still fails to load: " + (missing.missingLibrary ? missing.expectedIn : missing.symbol) : exit == QProcess::CrashExit ? "The launcher crashed." : "The app exited with status " + QString::number(code) + (code > 128 ? QString(" (") + strsignal(code - 128) + ")" : QString()));
    });
    QTimer::singleShot(stableSeconds * 1000, this, [this, prefix, env] {
        if (!running || process.state() == QProcess::NotRunning) return;
        const auto missing = LauncherCore::diagnose(output);
        auto *stop = new QProcess(this); stop->setProcessEnvironment(env);
        connect(stop, &QProcess::finished, stop, &QObject::deleteLater);
        stop->start(launcher, {"shutdown"});
        process.disconnect(this); process.terminate();
        if (missing.valid()) finish(false, "The app still fails to load: " + (missing.missingLibrary ? missing.expectedIn : missing.symbol));
        else finish(true, "The app stayed running for " + QString::number(stableSeconds) + " seconds in the prefix with no loader errors.");
    });
    process.start(launcher, {"exec", "/" + bundle + "/Contents/MacOS/" + executable});
}
BackgroundFix::BackgroundFix(const QString &agent, const QStringList &args, const QString &workspace, QObject *parent, const QJsonObject &diagnostic) : QObject(parent) {
    setObjectName("backgroundFix"); workspacePath = workspace; failure = diagnostic;
    process.setWorkingDirectory(workspace); process.setProcessChannelMode(QProcess::MergedChannels);
    auto append = [workspace](const QByteArray &bytes) { LauncherLog::write("agent", QString::fromUtf8(bytes)); QFile log(workspace + "/AGENT.log"); if (log.open(QIODevice::WriteOnly | QIODevice::Append)) log.write(bytes); };
    connect(&process, &QProcess::started, this, [this, agent, workspace] { process.closeWriteChannel(); emit statusChanged(agent + " is working in the background. Private log: " + workspace + "/AGENT.log"); });
    connect(&process, &QProcess::readyReadStandardOutput, this, [this, append] { append(process.readAllStandardOutput()); });
    connect(&process, &QProcess::finished, this, [this, append, agent, workspace, diagnostic](int code, QProcess::ExitStatus exit) {
        append(process.readAllStandardOutput());
        const bool success = exit == QProcess::NormalExit && code == 0;
        emit statusChanged(agent + (success ? " finished. Trying its fix inside the imported prefix before offering a PR…" : " stopped without a verified fix. Check its permissions/authentication and log in ") + workspace);
        if (!success || diagnostic.isEmpty()) return;
        emit verificationOffered();
    });
    connect(&process, &QProcess::errorOccurred, this, [this, agent](QProcess::ProcessError error) { if (error == QProcess::FailedToStart) emit statusChanged(agent + " failed to start: " + process.errorString()); });
    QTimer::singleShot(0, this, [this, agent, args] { if (!waiting) return; waiting = false; process.start(QStandardPaths::findExecutable(agent), args); });
}
void BackgroundFix::verify() {
    if (verifier && verifier->isRunning()) return;
    delete verifier; verifier = new FixVerifier(workspacePath, failure, 20, this);
    connect(verifier, &FixVerifier::finished, this, [this](const FixVerification &result) {
        emit statusChanged(result.verified ? "Fix verified inside the imported prefix. You can now review and submit the PR." : "The fix was not verified: " + result.reason);
        if (result.verified) emit verified(workspacePath + "/proposal.json");
    });
    verifier->start();
}
bool BackgroundFix::isRunning() const { return waiting || process.state() != QProcess::NotRunning || (verifier && verifier->isRunning()); }
BackgroundFix::~BackgroundFix() { process.disconnect(); }
void BackgroundFix::stop() {
    waiting = false; process.terminate();
    QTimer::singleShot(5000, this, [this] { if (process.state() != QProcess::NotRunning) process.kill(); });
    emit statusChanged("Stopping the owned agent CLI. Commands it already started may still finish in its private workspace.");
}
LauncherTroubleshooting::AgentTools LauncherTroubleshooting::prepareAgentTools(const QJsonObject &diagnostic, const QString &workspace, const QString &clone) {
    AgentTools tools;
    const QString prefix = diagnostic.value("prefix").toString(), runtime = diagnostic.value("runtime").toString(), launcher = diagnostic.value("launcher").toString(), bundle = diagnostic.value("bundle").toString(), executable = diagnostic.value("executable").toString();
    const QRegularExpression safe("^[A-Za-z0-9_./ +@%:=,-]+$");
    const QString lock = LauncherPrefix::heavyBuildLock(&tools.error);
    if (lock.isEmpty()) return tools;
    for (const QString &value : {prefix, runtime, launcher, bundle, executable, workspace, clone, lock})
        if (!value.isEmpty() && (!safe.match(value).hasMatch() || value.contains(".."))) { tools.error = "A path or app name contains characters that cannot be handed safely to an agent."; return tools; }
    tools.directory = LauncherDiscovery::dataRoot() + "/agent-tools/" + QFileInfo(workspace).fileName();
    if (!QDir().mkpath(tools.directory)) { tools.error = "Cannot create the agent tool directory."; return tools; }
    const QString q = "'";
    const QString backups = prefix + "/.darling-launcher/backups/" + QFileInfo(workspace).fileName();
    auto write = [&](const QString &name, const QString &body) {
        const QString path = tools.directory + "/" + name; QFile::remove(path); QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(("#!/bin/sh\nset -eu\n" + body).toUtf8()) < 0 || !file.commit() || !QFile::setPermissions(path, QFile::ReadOwner | QFile::ExeOwner)) { tools.error = "Cannot write " + path; return QString(); }
        return path;
    };
    tools.run = write("run-in-prefix.sh", "export DPREFIX=" + q + prefix + q + " DARLING_INSTALL_PREFIX=" + q + runtime + q + "\nexec timeout 60 " + q + launcher + q + " exec " + q + "/" + bundle + "/Contents/MacOS/" + executable + q + "\n");
    tools.build = write("build.sh", "case \"${1:-}\" in\n configure) exec cmake -S " + q + workspace + "/darling" + q + " -B " + q + workspace + "/build" + q + " -G Ninja ;;\n build) exec flock -w 1800 " + q + lock + q + " cmake --build " + q + workspace + "/build" + q + " --parallel 1 ;;\n *) echo 'usage: build.sh configure|build' >&2; exit 2 ;;\nesac\n");
    tools.deploy = write("deploy.sh", "[ $# -eq 2 ] || { echo 'usage: deploy.sh <file inside workspace> <relative path inside prefix>' >&2; exit 2; }\n"
        "src=$(realpath -e -- \"$1\"); rel=$2; pfx=$(realpath -m -- " + q + prefix + q + ")\n"
        "case \"$rel\" in /*|*..*) echo 'use a plain relative path inside the prefix' >&2; exit 2;; esac\n"
        "case \"$src\" in " + q + workspace + q + "/*) ;; *) echo 'source must be inside the agent workspace' >&2; exit 2;; esac\n"
        "exe=$(realpath -m -- \"$pfx\"/" + q + bundle + q + "/Contents/MacOS/" + q + executable + q + ")\n"
        "dest=\"$pfx/$rel\"; parent=$(realpath -m -- \"$(dirname -- \"$dest\")\"); full=\"$parent/$(basename -- \"$dest\")\"\n"
        "case \"$parent\" in \"$pfx\"|\"$pfx\"/*) ;; *) echo 'destination escapes the prefix' >&2; exit 2;; esac\n"
        "case \"$full\" in \"$pfx/.darling-launcher\"|\"$pfx/.darling-launcher/\"*|\"$exe\") echo 'refusing to replace launcher metadata or the failing app itself' >&2; exit 2;; esac\n"
        "backup=\"$pfx/.darling-launcher/backups/" + QFileInfo(workspace).fileName() + "/$rel\"\n"
        "if [ -e \"$full\" ] && [ ! -e \"$backup\" ]; then mkdir -p -- \"$(dirname -- \"$backup\")\"; cp -a -- \"$full\" \"$backup\"; fi\n"
        "mkdir -p -- \"$parent\"; [ ! -L \"$full\" ] || rm -f -- \"$full\"\n"
        "cp -a --remove-destination -- \"$src\" \"$full\"\nprintf '%s\\n' \"$rel\" >> " + q + workspace + "/PREFIX-CHANGES.txt" + q + "\n");
    tools.git = write("git.sh", "export GIT_CONFIG_COUNT=4 GIT_CONFIG_KEY_0=core.hooksPath GIT_CONFIG_VALUE_0=/dev/null GIT_CONFIG_KEY_1=core.fsmonitor GIT_CONFIG_VALUE_1=false GIT_CONFIG_KEY_2=core.pager GIT_CONFIG_VALUE_2=cat GIT_CONFIG_KEY_3=diff.external GIT_CONFIG_VALUE_3=\n"
        "unset GIT_EXTERNAL_DIFF GIT_DIR GIT_WORK_TREE GIT_CONFIG_GLOBAL\n"
        "[ $# -ge 1 ] || { echo 'usage: git.sh init-worktree | status|diff|log|show|add|rm|mv|commit|ls-files|rev-parse|branch|restore ...' >&2; exit 2; }\n"
        "sub=$1; shift\n"
        "if [ \"$sub\" = init-worktree ]; then exec git -C " + q + clone + q + " worktree add -b fix/" + QFileInfo(workspace).fileName().left(8) + " " + q + workspace + "/darling" + q + "; fi\n"
        "for a in \"$@\"; do case \"$a\" in -c*|-C*|--git-dir*|--work-tree*|--exec-path*|--output*|--ext-diff|--textconv|--no-index|--upload-pack*|--receive-pack*|--open-files-in-pager*|--template*) echo \"refused option: $a\" >&2; exit 2;; esac; done\n"
        "case \"$sub\" in status|diff|log|show|add|rm|mv|commit|ls-files|rev-parse|branch|restore) ;; *) echo \"refused git subcommand: $sub\" >&2; exit 2;; esac\n"
        "cd " + q + workspace + "/darling" + q + " && exec git \"$sub\" \"$@\"\n");
    if (tools.error.isEmpty() && (tools.run.isEmpty() || tools.build.isEmpty() || tools.deploy.isEmpty() || tools.git.isEmpty())) tools.error = "Cannot prepare the agent tools.";
    return tools;
}
QString LauncherTroubleshooting::trustedWorktree(const QString &workspace, const QString &candidate) {
    const QString root = QFileInfo(workspace).canonicalFilePath(), source = QFileInfo(candidate).canonicalFilePath();
    if (root.isEmpty() || source.isEmpty() || !source.startsWith(root + '/')) return {};
    const QFileInfo dotGit(source + "/.git");
    if (dotGit.isSymLink()) return {};
    if (dotGit.isDir()) return source;
    QFile pointer(dotGit.filePath());
    if (!pointer.open(QIODevice::ReadOnly)) return {};
    const QString line = QString::fromUtf8(pointer.readLine(4096)).trimmed();
    if (!line.startsWith("gitdir: ")) return {};
    const QString gitdir = QFileInfo(line.mid(8)).canonicalFilePath(), common = QFileInfo(LauncherDiscovery::dataRoot() + "/sources/vibedarling/.git/worktrees").canonicalFilePath();
    return !gitdir.isEmpty() && !common.isEmpty() && gitdir.startsWith(common + '/') ? source : QString();
}
QString LauncherTroubleshooting::verificationMac(const QString &workspace, const QString &commit, bool verified) {
    const QString keyPath = LauncherDiscovery::dataRoot() + "/.verification-key";
    QByteArray key; { QFile in(keyPath); if (in.open(QIODevice::ReadOnly)) key = in.readAll().trimmed(); }
    if (key.size() < 64) {
        key.clear(); for (int i = 0; i < 4; ++i) key += QByteArray::number(QRandomGenerator::system()->generate64(), 16).rightJustified(16, '0');
        if (!QDir().mkpath(QFileInfo(keyPath).absolutePath())) return {};
        QSaveFile out(keyPath); if (!out.open(QIODevice::WriteOnly) || out.write(key) < 0 || !out.commit()) return {};
        QFile::setPermissions(keyPath, QFile::ReadOwner | QFile::WriteOwner);
    }
    const QByteArray message = (QFileInfo(workspace).canonicalFilePath() + '\n' + commit + '\n' + (verified ? "verified" : "failed")).toUtf8();
    return QString::fromLatin1(QMessageAuthenticationCode::hash(message, key, QCryptographicHash::Sha256).toHex());
}
QString LauncherTroubleshooting::verificationFile(const QString &workspace, const QString &commit) {
    const QByteArray key = (QFileInfo(workspace).canonicalFilePath() + '\n' + commit).toUtf8();
    return LauncherDiscovery::dataRoot() + "/verifications/" + QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex()) + ".json";
}
PrProposal LauncherTroubleshooting::reviewProposal(const QString &file) {
    PrProposal result; QFile input(file);
    if (!input.open(QIODevice::ReadOnly)) { result.error = input.errorString(); return result; }
    result.fields = QJsonDocument::fromJson(input.readAll()).object();
    for (const QString &field : {"source", "repo", "base", "head", "title", "body"})
        if (result.fields.value(field).toString().trimmed().isEmpty()) { result.error = "Completed proposal requires source, repo, base, head, title and body."; return result; }
    const auto fields = result.fields;
    const QString workspace = QFileInfo(file).absolutePath();
    const QString source = LauncherTroubleshooting::trustedWorktree(workspace, fields.value("source").toString());
    if (source.isEmpty()) { result.error = "Source must be a worktree of the cloned Darling source inside this proposal's workspace."; return result; }
    QRegularExpression repo("^VibeDarling/[A-Za-z0-9_.-]+$"), head("^([A-Za-z0-9_.-]+):([A-Za-z0-9][A-Za-z0-9_./-]*)$"), branch("^[A-Za-z0-9][A-Za-z0-9_./-]*$");
    const auto selectedHead = head.match(fields.value("head").toString());
    if (!repo.match(fields.value("repo").toString()).hasMatch() || !selectedHead.hasMatch() || !branch.match(fields.value("base").toString()).hasMatch()) { result.error = "Use a VibeDarling target repository, an explicit base branch and owner:branch head."; return result; }
    QString git = QStandardPaths::findExecutable("git"); bool ok;
    auto run = [&](const QStringList &args) { return query(git, QStringList{"-C", source} + args, &ok); };
    const QString dirty = run({"status", "--porcelain"});
    if (!ok || !dirty.isEmpty()) { result.error = "Commit and review the source changes in a clean independent clone first."; return result; }
    if (run({"symbolic-ref", "--short", "HEAD"}) != selectedHead.captured(2) || !ok) { result.error = "Proposal head must match the source clone's current branch."; return result; }
    result.commit = run({"rev-parse", "HEAD"});
    if (!ok || result.commit.isEmpty()) { result.error = "Cannot identify source commit."; return result; }
    QFile verification(LauncherTroubleshooting::verificationFile(workspace, result.commit));
    const auto verified = verification.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(verification.readAll()).object() : QJsonObject();
    if (!verified.value("verified").toBool() || verified.value("mac").toString() != LauncherTroubleshooting::verificationMac(workspace, result.commit, true)) { result.error = "No PR is offered until the fix is verified: the launcher has not run the app successfully with this patch in the imported prefix."; return result; }
    if (verified.value("proposalCommit").toString() != result.commit) { result.error = "The patch changed after it was verified. Verify the current commit before offering a PR."; return result; }
    const QString range = fields.value("base").toString() + "...HEAD";
    const QString numbers = run({"diff", "--numstat", "--no-ext-diff", "--no-textconv", range});
    if (!ok || numbers.isEmpty() || QRegularExpression("(^|\\n)-\\t").match(numbers).hasMatch()) { result.error = "Proposal must contain source changes and no binary patches."; return result; }
    result.patch = run({"diff", "--no-ext-diff", "--no-textconv", range});
    if (!ok || result.patch.isEmpty()) result.error = "Cannot read the complete proposed source patch.";
    result.fields.insert("source", source);
    return result;
}
RecoveryChoices LauncherTroubleshooting::recoveryChoices() {
    QSettings settings("cristim", "darling-launcher");
    return {settings.value("recovery/import", false).toBool(), settings.value("recovery/report", false).toBool(), settings.value("recovery/ai", false).toBool(), settings.value("recovery/remember", false).toBool(), settings.value("recovery/agent").toString()};
}
void LauncherTroubleshooting::saveRecoveryChoices(const RecoveryChoices &choices) {
    QSettings settings("cristim", "darling-launcher");
    settings.setValue("recovery/import", choices.importLibrary); settings.setValue("recovery/report", choices.report);
    settings.setValue("recovery/ai", choices.ai); settings.setValue("recovery/remember", choices.remember); settings.setValue("recovery/agent", choices.agent);
}
LaunchChoicesDialog::LaunchChoicesDialog(const QString &app, bool mounted, QWidget *parent, bool failed) : QDialog(parent) {
    setObjectName("launchChoicesDialog"); setWindowTitle("Launch recovery choices");
    auto *layout = new QVBoxLayout(this);
    setProperty("failurePreferences", failed);
    auto *notice = new QLabel((failed ? app + " failed to launch. Choose recovery actions for this failure." : "Before launching " + app + ", choose what to do if a dependency is missing.") + " These choices are also available later in Settings."); notice->setWordWrap(true); notice->setTextFormat(Qt::PlainText); layout->addWidget(notice);
    copy = new QCheckBox("Import missing libraries from mounted macOS into this private prefix and retry"); copy->setObjectName("launchImportOption"); layout->addWidget(copy);
    mount = new QPushButton("Mount or select macOS source…"); mount->setObjectName("launchMountSource"); layout->addWidget(mount); connect(mount, &QPushButton::clicked, this, [this] { reject(); emit mountRequested(); });
    report = new QCheckBox("Prepare an issue draft using my GitHub account; ask before submitting the completed draft"); report->setObjectName("launchReportOption"); layout->addWidget(report);
    ai = new QCheckBox("Use AI in the background to implement missing functionality"); ai->setObjectName("launchAiOption"); layout->addWidget(ai);
    agent = new QComboBox; agent->setObjectName("launchAgent"); agent->addItems(LauncherTroubleshooting::agents()); layout->addWidget(agent); ai->setEnabled(agent->count() > 0);
    auto *sharing = new QLabel("Choosing AI authorizes sharing subsequent loader output and local app/source/prefix paths with the selected agent’s configured service. No Apple payloads are shared. Work uses an independent workspace; completed issues and PRs still require separate explicit approval. The same actions cover the dependency chain, with a separate AI session for each missing library. Library import waits if the prefix must be stopped first."); sharing->setWordWrap(true); layout->addWidget(sharing);
    remember = new QCheckBox("Remember these choices for subsequent launches"); remember->setObjectName("rememberLaunchChoices"); layout->addWidget(remember);
    const auto saved = LauncherTroubleshooting::recoveryChoices(); copy->setChecked(saved.importLibrary); report->setChecked(saved.report); ai->setChecked(saved.ai && ai->isEnabled()); remember->setChecked(saved.remember);
    if (agent->findText(saved.agent) >= 0) agent->setCurrentText(saved.agent);
    setMounted(mounted);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); layout->addWidget(buttons); connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}
RecoveryChoices LaunchChoicesDialog::choices() const { return {copy->isChecked(), report->isChecked(), ai->isChecked(), remember->isChecked(), agent->currentText()}; }
void LaunchChoicesDialog::setMounted(bool mounted) { setSourceAvailability(mounted, macOSPossible); }
void LaunchChoicesDialog::setSourceAvailability(bool mounted, bool possible) {
    macOSPossible = possible; copy->setVisible(mounted || !possible); copy->setEnabled(mounted);
    copy->setToolTip(possible ? QString() : "No APFS/HFS partition or usable macOS mount detected on this system.");
    mount->setVisible(!mounted && possible);
}
void TroubleshootingDialog::chooseActions(bool copy, bool report, bool ai, const QString &selectedAgent) {
    if (ai) {
        auto *workspace = findChild<QLineEdit *>("agentWorkspace");
        if (QFileInfo::exists(workspace->text())) workspace->setText(QFileInfo(workspace->text()).absolutePath() + '/' + QUuid::createUuid().toString(QUuid::WithoutBraces));
    }
    if (!importOption) {
        if (report) reviewIssue();
        if (ai) {
            auto *agents = findChild<QComboBox *>("agentChoices");
            if (agents->findText(selectedAgent) >= 0) {
                agents->setCurrentText(selectedAgent); findChild<QCheckBox *>("backgroundAgent")->setChecked(true); findChild<QCheckBox *>("approveAgentData")->setChecked(true);
                findChild<QPushButton *>("startFixAgent")->click();
            }
        }
        return;
    }
    if (auto *agents = findChild<QComboBox *>("agentChoices")) {
        if (agents->findText(selectedAgent) >= 0) agents->setCurrentText(selectedAgent);
        else ai = false;
    }
    importOption->setChecked(copy && importOption->isEnabled()); issueOption->setChecked(report); aiOption->setChecked(ai && aiOption->isEnabled());
    if (aiOption->isChecked()) findChild<QCheckBox *>("approveAgentData")->setChecked(true);
    applyingChoices = true;
    findChild<QPushButton *>("runDependencyActions")->click();
    applyingChoices = false;
}
TroubleshootingDialog::TroubleshootingDialog(const QJsonObject &initialData, QWidget *parent) : QDialog(parent), diagnostic(initialData) {
    setObjectName("troubleshootingDialog"); setWindowTitle("Troubleshoot app failure"); resize(780, 600);
    auto *layout = new QVBoxLayout(this);
    summary = new QLabel; summary->setObjectName("dependencySummary"); summary->setWordWrap(true); summary->setTextFormat(Qt::PlainText); layout->addWidget(summary);
    outcome = new QLabel; outcome->setObjectName("dependencyOutcome"); outcome->setWordWrap(true); outcome->setTextFormat(Qt::PlainText); layout->addWidget(outcome);
    const bool missing = !diagnostic.value("missingLibrary").toString().isEmpty();
    if (missing) {
        setWindowTitle("Missing dependency — " + diagnostic.value("app").toString());
        importOption = new QCheckBox("Import the missing library from my mounted macOS source and retry"); importOption->setObjectName("importDependencyOption"); layout->addWidget(importOption);
        mountSource = new QPushButton("Mount or select macOS source…"); mountSource->setObjectName("mountDependencySource"); layout->addWidget(mountSource); connect(mountSource, &QPushButton::clicked, this, &TroubleshootingDialog::mountRequested);
        issueOption = new QCheckBox("Report an issue — prepare the completed draft for review"); issueOption->setObjectName("reportIssueOption"); layout->addWidget(issueOption);
        aiOption = new QCheckBox("Use AI to implement the missing library or symbols in the background"); aiOption->setObjectName("aiFixOption"); layout->addWidget(aiOption);
        stopPrefix = new QPushButton("Stop processes in this prefix"); stopPrefix->setObjectName("stopDependencyPrefix"); layout->addWidget(stopPrefix); connect(stopPrefix, &QPushButton::clicked, this, &TroubleshootingDialog::stopRequested);
    }
    auto *choices = new QLabel("Local workaround: import the exact missing library into this private prefix and retry from the launcher.\nIssue draft: save reproduction steps and dependency provenance for your review; saving does not submit it.\nSource fix: start an installed agent in a separate workspace using the troubleshooting data shown below."); choices->setObjectName("contributionChoices"); choices->setWordWrap(true); layout->addWidget(choices);
    auto *saveIssue = new QPushButton("Save local issue draft…"); saveIssue->setObjectName("saveFailureIssue"); layout->addWidget(saveIssue);
    connect(saveIssue, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getSaveFileName(this, "Save proposed issue", "proposed-vibedarling-issue.md", "Markdown (*.md)");
        if (file.isEmpty()) return;
        QSaveFile output(file);
        const AppEntry app{diagnostic.value("app").toString(), diagnostic.value("bundle").toString(), diagnostic.value("executable").toString(), diagnostic.value("sourceBundle").toString()};
        const QString body = LauncherCore::issueDraft(app, diagnostic.value("dependencyChain").toArray(), diagnostic.value("loaderOutput").toString(), diagnostic.value("sourceVolume").toString(), diagnostic.value("prefix").toString(), diagnostic.value("launcher").toString(), diagnostic.value("runtime").toString());
        if (!output.open(QIODevice::WriteOnly) || output.write(body.toUtf8()) < 0 || !output.commit()) QMessageBox::warning(this, "Issue draft", "Cannot save the local draft.");
    });
    auto *notice = new QLabel("Choose an installed agent for a separate clean-room source fix. Starting it shares loader output and local path provenance with that tool’s configured service. No Apple payloads are included. Work stays in a new workspace; no issue, push or PR is automatic."); notice->setWordWrap(true); layout->addWidget(notice);
    details = new QTextEdit; details->setReadOnly(true); details->setObjectName("troubleshootingData"); details->setPlainText(QString::fromUtf8(QJsonDocument(diagnostic).toJson())); layout->addWidget(details);
    auto *toggleDetails = new QPushButton("Show details"); toggleDetails->setObjectName("showDependencyDetails"); toggleDetails->setCheckable(true); layout->insertWidget(layout->indexOf(details), toggleDetails); details->hide();
    connect(toggleDetails, &QPushButton::toggled, this, [this, toggleDetails](bool shown) { details->setVisible(shown); toggleDetails->setText(shown ? "Hide details" : "Show details"); });
    auto *agentPanel = new QGroupBox("Clean-room source fix"); auto *agentLayout = new QVBoxLayout(agentPanel); layout->addWidget(agentPanel);
    auto *agents = new QComboBox; agents->setObjectName("agentChoices"); agents->addItems(LauncherTroubleshooting::agents()); agentLayout->addWidget(agents);
    auto *workspace = new QLineEdit(LauncherDiscovery::dataRoot() + "/source-fixes/" + QUuid::createUuid().toString(QUuid::WithoutBraces)); workspace->setObjectName("agentWorkspace"); workspace->setParent(agentPanel); workspace->hide();
    auto *consent = new QCheckBox("I approve sharing the displayed troubleshooting data with the selected agent’s configured service."); consent->setObjectName("approveAgentData"); agentLayout->addWidget(consent);
    auto *background = new QCheckBox("Run in the background without opening a terminal"); background->setObjectName("backgroundAgent"); background->setChecked(missing); agentLayout->addWidget(background);
    auto *start = new QPushButton("Start selected agent"); start->setObjectName("startFixAgent"); start->setEnabled(false);
    connect(consent, &QCheckBox::toggled, this, [=](bool approved) { start->setEnabled(approved && agents->count() > 0 && (background->isChecked() || !QStandardPaths::findExecutable("xdg-terminal-exec").isEmpty())); }); agentLayout->addWidget(start);
    connect(background, &QCheckBox::toggled, this, [=] { start->setEnabled(consent->isChecked() && agents->count() > 0 && (background->isChecked() || !QStandardPaths::findExecutable("xdg-terminal-exec").isEmpty())); });
    status = new QLabel; status->setObjectName("troubleshootingStatus"); status->setWordWrap(true); layout->addWidget(status);
    connect(start, &QPushButton::clicked, this, [=] {
        if (!consent->isChecked()) return;
        QString directory = QDir::cleanPath(workspace->text());
        QString parent = QFileInfo(directory).absolutePath(); QString suffix = QFileInfo(directory).fileName();
        while (!QFileInfo::exists(parent) && parent != "/") { suffix = QFileInfo(parent).fileName() + '/' + suffix; parent = QFileInfo(parent).absolutePath(); }
        const QString resolved = QFileInfo(parent).canonicalFilePath() + '/' + suffix;
        if (!QDir::isAbsolutePath(directory) || directory == "/" || QFileInfo::exists(directory) || QFileInfo(directory).isSymLink()) { status->setText("Choose a new absolute workspace directory."); return; }
        for (const QString &field : {"prefix", "sourceVolume"}) {
            QString confined = QFileInfo(diagnostic.value(field).toString()).canonicalFilePath();
            if (!confined.isEmpty() && (directory == confined || directory.startsWith(confined + '/') || resolved == confined || resolved.startsWith(confined + '/'))) { status->setText("Workspace must be outside prefixes and the mounted macOS volume."); return; }
        }
        if (!QDir().mkpath(directory)) { status->setText("Cannot create source-fix workspace."); return; }
        const QString canonical = QFileInfo(directory).canonicalFilePath();
        for (const QString &field : {"prefix", "sourceVolume"}) {
            QString confined = QFileInfo(diagnostic.value(field).toString()).canonicalFilePath();
            if (!confined.isEmpty() && (canonical == confined || canonical.startsWith(confined + '/'))) { status->setText("Workspace parent resolves inside a prefix or source volume."); return; }
        }
        QSaveFile report(directory + "/TROUBLESHOOTING.json");
        if (!report.open(QIODevice::WriteOnly) || report.write(QJsonDocument(diagnostic).toJson()) < 0 || !report.commit()) { status->setText("Cannot save diagnostic report."); return; }
        const QString prefixPath = diagnostic.value("prefix").toString(), runtimePath = diagnostic.value("runtime").toString(), launcherPath = diagnostic.value("launcher").toString(), bundlePath = diagnostic.value("bundle").toString(), executableName = diagnostic.value("executable").toString();
        const QString clone = LauncherDiscovery::dataRoot() + "/sources/vibedarling";
        const auto agentTools = LauncherTroubleshooting::prepareAgentTools(diagnostic, directory, clone);
        if (!agentTools.valid()) { status->setText(agentTools.error); return; }
        const QString instructions = "Use TROUBLESHOOTING.json (exit status, signal and loader output of the failed launch) to investigate and fix this Darling app failure. "
            "WORK IN THE IMPORTED PREFIX: the prefix is " + prefixPath + " and the failing app is /" + bundlePath + ". Reproduce the failure exactly as the launcher runs it with `" + agentTools.run + "` (it sets the prefix and runtime and applies a 60 second timeout). "
            "Fix it in place as far as you can WITHOUT IMPACTING OTHER APPS: prefer changes scoped to this app (app-local libraries, per-app environment); if a shared file in the prefix must change, first back up the original under " + prefixPath + "/.darling-launcher/backups/ keeping its relative path, and list every changed prefix path in " + directory + "/PREFIX-CHANGES.txt so it can be reverted. Never change /usr/local, the system runtime, other prefixes or other imported apps. "
            "THE SOURCE IS ALREADY CLONED at " + clone + ". Do not clone it again and do not modify that checkout directly: create your own git worktree on it with `" + agentTools.git + " init-worktree` and make and commit your source changes there with `" + agentTools.git + " <status|diff|log|show|add|rm|mv|commit|branch|restore> ...` (it works inside " + directory + "/darling only). Read its AGENTS.md. Configure and build with `" + agentTools.build + " configure` and `" + agentTools.build + " build` (it builds in " + directory + "/build under the shared heavy-build lock) and deploy built files back into the prefix one at a time with `" + agentTools.deploy + " <file inside the workspace> <relative path inside the prefix>`, which backs up the original and records the change. These four scripts are the only commands you may run. "
            "Work only from source, published APIs, headers, interface metadata, loader output and process exit status. Never disassemble, decompile, inspect machine code, dump Apple symbols or commit Apple apps/libraries; `coredumpctl info` module names are fine but do not open core dumps in a debugger. Do not inspect dyld cache implementation bytes. Do not unlock or change encrypted volumes. "
            "Add meaningful tests, prepare a completed local PR draft, and do not push or submit any issue or PR. If a fix cannot be specified without binary inspection, explain the blocker. "
            "Write proposal.json beside this report with string fields source (absolute worktree path " + directory + "/darling), repo (VibeDarling/repo), base (explicit branch), head (fork-owner:branch), title, body, and commit the reviewed patch on that branch. The branch must be published separately with explicit user approval before PR submission can succeed. "
            "When done the launcher itself runs the app in the prefix for about 20 seconds; a PR is offered only if that run succeeds, so do not claim success you have not seen.";
        QSaveFile prompt(directory + "/FIX-INSTRUCTIONS.txt");
        if (!prompt.open(QIODevice::WriteOnly) || prompt.write(instructions.toUtf8()) < 0 || !prompt.commit()) { status->setText("Cannot save agent instructions."); return; }
        const QString chosen = agents->currentText(), executable = QStandardPaths::findExecutable(chosen);
        const auto arguments = background->isChecked() ? LauncherTroubleshooting::backgroundArguments(chosen, instructions, {{prefixPath, directory}, {"Bash(" + agentTools.git + ":*)", "Bash(" + agentTools.run + ":*)", "Bash(" + agentTools.build + ":*)", "Bash(" + agentTools.deploy + ":*)"}}) : LauncherTroubleshooting::agentArguments(chosen, instructions);
        if (executable.isEmpty() || arguments.isEmpty()) { status->setText("Selected agent is no longer installed."); return; }
        if (background->isChecked()) {
            auto *job = new BackgroundFix(chosen, arguments, directory, parentWidget() ? parentWidget() : this, diagnostic);
            job->setProperty("workspace", directory); job->setProperty("library", diagnostic.value("missingLibrary").toString());
            connect(job, &BackgroundFix::statusChanged, this, [this](const QString &message) { status->setText(message); });
            if (auto *window = qobject_cast<QMainWindow *>(parentWidget())) connect(job, &BackgroundFix::statusChanged, window, [window](const QString &message) { window->statusBar()->showMessage(message); });
            const QJsonObject failure = diagnostic; QWidget *window = parentWidget();
            connect(job, &BackgroundFix::verificationOffered, job, [window, job] {
                auto *box = new QMessageBox(QMessageBox::Question, "Try the agent's fix", "The agent finished working in " + job->property("workspace").toString() + ".\n\nTry its fix now? The launcher will run the app in your imported prefix for about 20 seconds.", QMessageBox::Yes | QMessageBox::No, window);
                box->setAttribute(Qt::WA_DeleteOnClose);
                connect(box, &QDialog::finished, job, [window, job](int answer) {
                    if (answer == QMessageBox::Yes) job->verify(); else if (auto *main = qobject_cast<QMainWindow *>(window)) main->statusBar()->showMessage("Fix not tried; no PR is offered until it has been verified in the prefix.");
                });
                box->open();
            });
            connect(job, &BackgroundFix::verified, job, [window, failure](const QString &proposalFile) {
                if (QMessageBox::question(window, "Fix verified", "The fix ran successfully inside your imported prefix.\n\nReview the completed patch and PR draft now? Nothing is submitted until you approve the completed draft.") != QMessageBox::Yes) return;
                auto *review = new TroubleshootingDialog(failure, window); review->setAttribute(Qt::WA_DeleteOnClose); review->show(); review->loadProposal(proposalFile);
            });
            status->setText("Starting background agent in " + directory); return;
        }
        const bool launched = QProcess::startDetached(QStandardPaths::findExecutable("xdg-terminal-exec"), QStringList{"--dir=" + directory, "--", executable} + arguments, directory);
        status->setText(launched ? "Agent started in " + directory + ". Review its patch and completed draft before publishing anything." : "Could not open the selected agent in a terminal.");
    });
    if (missing) {
        start->hide(); agentPanel->hide(); saveIssue->hide(); choices->hide(); notice->hide();
        aiOption->setEnabled(agents->count() > 0); aiOption->setToolTip(agents->count() ? "Choose an agent and approve the displayed data before starting" : "Install Claude, Codex or OpenCode to enable source fixes");
        connect(aiOption, &QCheckBox::toggled, agentPanel, &QWidget::setVisible);
        connect(aiOption, &QCheckBox::toggled, notice, &QWidget::setVisible);
        auto *apply = new QPushButton("Run selected actions"); apply->setObjectName("runDependencyActions"); apply->setEnabled(false); layout->addWidget(apply);
        auto updateActions = [=] { apply->setEnabled((importOption->isChecked() || issueOption->isChecked() || aiOption->isChecked()) && (!aiOption->isChecked() || start->isEnabled())); };
        for (auto *option : {importOption, issueOption, aiOption, consent}) connect(option, &QCheckBox::toggled, this, updateActions);
        connect(apply, &QPushButton::clicked, this, [=] {
            const bool copy = importOption->isChecked(), report = issueOption->isChecked(), fix = aiOption->isChecked();
            if (fix && !start->isEnabled()) return;
            if (!applyingChoices) emit actionsChosen(copy, report, fix, agents->currentText());
            if (fix) start->click();
            if (copy) emit importRequested();
            if (report) reviewIssue();
            importOption->setChecked(false); issueOption->setChecked(false); aiOption->setChecked(false); consent->setChecked(false);
        });
    }
    updateDiagnostic(diagnostic, false, false);
    review = new QPushButton("Review completed PR proposal…"); review->setObjectName("reviewPrProposal"); review->setEnabled(false); layout->addWidget(review);
    connect(review, &QPushButton::clicked, this, [this] { const QString file = QFileDialog::getOpenFileName(this, "Choose completed proposal.json", {}, "JSON (*.json)"); if (!file.isEmpty()) loadProposal(file); });
    gh = QStandardPaths::findExecutable("gh");
    if (gh.isEmpty()) status->setText("GitHub CLI is not installed. Agent work and local drafts are available.");
    else {
        auto *check = new QProcess(this);
        connect(check, &QProcess::finished, this, [this, check](int code, QProcess::ExitStatus exit) { authenticated = code == 0 && exit == QProcess::NormalExit; review->setEnabled(authenticated); for (auto *button : findChildren<QPushButton *>("approveCompletedIssue")) button->setEnabled(authenticated && !button->parent()->property("draftStale").toBool() && !button->parent()->property("submitted").toBool()); status->setText(authenticated ? "GitHub CLI authenticated. Completed PR drafts can be reviewed and explicitly approved here." : "GitHub CLI is not authenticated. Sign in separately before submitting a reviewed PR."); check->deleteLater(); });
        connect(check, &QProcess::errorOccurred, this, [this, check](QProcess::ProcessError error) { if (error == QProcess::FailedToStart) { status->setText("Cannot start GitHub CLI authentication check."); check->deleteLater(); } });
        QTimer::singleShot(15000, check, [this, check] { if (check->state() != QProcess::NotRunning) { status->setText("GitHub CLI authentication check timed out."); check->kill(); } });
        check->start(gh, {"auth", "status", "--hostname", "github.com"});
    }
    auto *close = new QPushButton("Later"); layout->addWidget(close); connect(close, &QPushButton::clicked, this, &QDialog::reject);
}
TroubleshootingDialog::~TroubleshootingDialog() {
    for (auto *process : findChildren<QProcess *>()) process->disconnect();
}
void TroubleshootingDialog::done(int result) {
    for (auto *dialog : findChildren<QDialog *>("issueApprovalDialog"))
        for (auto *process : dialog->findChildren<QProcess *>()) if (process->state() != QProcess::NotRunning) { status->setText("Wait for the approved issue submission to finish before closing."); return; }
    QDialog::done(result);
}
void TroubleshootingDialog::updateDiagnostic(const QJsonObject &data, bool canImport, bool launchRunning, const QString &result) {
    if (diagnostic != data) if (auto *consent = findChild<QCheckBox *>("approveAgentData")) consent->setChecked(false);
    diagnostic = data;
    updateIssueDrafts();
    details->setPlainText(QString::fromUtf8(QJsonDocument(data).toJson()));
    const QString symbol = data.value("missingSymbol").toString(), library = data.value("missingLibrary").toString();
    summary->setText(library.isEmpty() ? data.value("app").toString() + " troubleshooting" : data.value("app").toString() + " needs " + library + (symbol.isEmpty() ? QString() : "\nUnresolved symbol: " + symbol));
    outcome->setText(result);
    const bool mounted = data.value("sourceMounted").toBool();
    const bool possible = data.value("sourceAvailable").toBool(true);
    if (mountSource) mountSource->setVisible(!mounted && possible);
    if (importOption) { importOption->setVisible(mounted || !possible); importOption->setEnabled(canImport && mounted); if (!canImport || !mounted) importOption->setChecked(false); importOption->setToolTip(!possible ? "No APFS/HFS partition or usable macOS mount detected on this system." : launchRunning ? "Stop the stalled launch process explicitly before importing and retrying. This affects all apps in this prefix." : "Import the exact standalone library from the displayed macOS source into the displayed private prefix."); }
    if (stopPrefix) stopPrefix->setVisible(launchRunning);
}
QString TroubleshootingDialog::issueBody() const {
    const AppEntry app{diagnostic.value("app").toString(), diagnostic.value("bundle").toString(), diagnostic.value("executable").toString(), diagnostic.value("sourceBundle").toString()};
    return LauncherCore::issueDraft(app, diagnostic.value("dependencyChain").toArray(), diagnostic.value("loaderOutput").toString(), diagnostic.value("sourceVolume").toString(), diagnostic.value("prefix").toString(), diagnostic.value("launcher").toString(), diagnostic.value("runtime").toString());
}
void TroubleshootingDialog::updateIssueDrafts() {
    const QString latest = issueBody();
    for (auto *dialog : findChildren<QDialog *>("issueApprovalDialog")) {
        if (dialog->property("submitted").toBool()) continue;
        auto *body = dialog->findChild<QTextEdit *>("issueBody");
        if (dialog->property("latestBody").toString() == latest) continue;
        dialog->setProperty("latestBody", latest);
        if (!body->document()->isModified()) body->setPlainText(latest);
        else {
            dialog->findChild<QLabel *>("issueSubmissionStatus")->setText("Dependency chain changed. Your edits were preserved; use Refresh draft to review the latest provenance before submitting.");
            dialog->setProperty("draftStale", true); dialog->findChild<QPushButton *>("refreshIssueDraft")->show(); dialog->findChild<QPushButton *>("approveCompletedIssue")->setEnabled(false);
        }
    }
}
void TroubleshootingDialog::reviewIssue() {
    auto *dialog = new IssueApprovalDialog(this); dialog->setObjectName("issueApprovalDialog"); dialog->setWindowTitle("Review completed issue draft"); dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->resize(820, 650);
    auto *layout = new QVBoxLayout(dialog);
    auto *notice = new QLabel("Review the complete draft and redact local paths. Selecting Report prepares this draft; only approving this completed draft submits it."); notice->setWordWrap(true); layout->addWidget(notice);
    auto *repo = new QLineEdit("VibeDarling/Darling"); repo->setObjectName("issueRepository"); repo->setReadOnly(true); layout->addWidget(repo);
    auto *title = new QLineEdit(diagnostic.value("app").toString() + " missing loader dependency"); title->setObjectName("issueTitle"); layout->addWidget(title);
    auto *body = new QTextEdit; body->setObjectName("issueBody"); body->setPlainText(issueBody()); layout->addWidget(body); dialog->setProperty("latestBody", issueBody());
    auto *result = new QLabel; result->setObjectName("issueSubmissionStatus"); result->setWordWrap(true); layout->addWidget(result);
    auto *save = new QPushButton("Save local draft…"); layout->addWidget(save);
    connect(save, &QPushButton::clicked, dialog, [=] { const auto file = QFileDialog::getSaveFileName(dialog, "Save reviewed issue", "proposed-issue.md", "Markdown (*.md)"); if (file.isEmpty()) return; QSaveFile output(file); if (!output.open(QIODevice::WriteOnly) || output.write(body->toPlainText().toUtf8()) < 0 || !output.commit()) result->setText("Cannot save local draft."); else result->setText("Draft saved locally. Nothing submitted."); });
    auto *approve = new QPushButton("Approve completed issue and submit"); approve->setObjectName("approveCompletedIssue"); approve->setEnabled(authenticated); layout->addWidget(approve);
    auto *refresh = new QPushButton("Refresh draft with latest dependency chain (replaces edits)"); refresh->setObjectName("refreshIssueDraft"); layout->addWidget(refresh); refresh->hide();
    connect(refresh, &QPushButton::clicked, dialog, [=] { body->setPlainText(issueBody()); dialog->setProperty("latestBody", issueBody()); dialog->setProperty("draftStale", false); refresh->hide(); approve->setEnabled(authenticated); result->setText("Latest dependency chain loaded. Review and redact this completed draft before approval."); });
    if (!authenticated) result->setText("GitHub CLI is not authenticated. You can save the draft locally.");
    connect(approve, &QPushButton::clicked, dialog, [=] {
        if (!authenticated || dialog->property("draftStale").toBool()) return;
        if (title->text().trimmed().isEmpty() || body->toPlainText().trimmed().isEmpty()) { result->setText("Complete the title and body for VibeDarling/Darling."); return; }
        auto *file = new QTemporaryFile(dialog); if (!file->open() || file->write(body->toPlainText().toUtf8()) < 0 || !file->flush()) { result->setText("Cannot prepare approved draft."); return; }
        auto *submit = new QProcess(dialog); dialog->setProperty("submitted", true); approve->setEnabled(false); result->setText("Submitting approved issue…");
        connect(submit, &QProcess::finished, dialog, [=](int code, QProcess::ExitStatus exit) { result->setText(code == 0 && exit == QProcess::NormalExit ? "Issue submitted: " + QString::fromUtf8(submit->readAllStandardOutput()).trimmed() : "Issue submission failed: " + QString::fromUtf8(submit->readAllStandardError()).trimmed()); submit->deleteLater(); file->deleteLater(); });
        connect(submit, &QProcess::errorOccurred, dialog, [=](QProcess::ProcessError error) { if (error == QProcess::FailedToStart) { result->setText("Cannot start GitHub CLI. No issue submitted."); submit->deleteLater(); file->deleteLater(); } });
        submit->start(gh, {"issue", "create", "--repo", "VibeDarling/Darling", "--title", title->text(), "--body-file", file->fileName()});
    });
    auto *close = new QPushButton("Close"); layout->addWidget(close); connect(close, &QPushButton::clicked, dialog, &QDialog::reject); dialog->show();
}
void TroubleshootingDialog::loadProposal(const QString &file) {
    if (!authenticated) { status->setText("Authenticate GitHub CLI before reviewing a PR for submission."); return; }
    auto *watcher = new QFutureWatcher<PrProposal>(this); review->setEnabled(false); status->setText("Checking the completed source patch…");
    connect(watcher, &QFutureWatcher<PrProposal>::finished, this, [this, watcher] {
        const auto proposal = watcher->result(); watcher->deleteLater(); review->setEnabled(authenticated);
        if (!proposal.valid()) { status->setText(proposal.error); return; }
        QDialog dialog(this); dialog.setObjectName("prApprovalDialog"); dialog.setWindowTitle("Review completed PR and source patch"); dialog.resize(900, 700); QVBoxLayout layout(&dialog);
        QTextEdit text; text.setReadOnly(true); text.setPlainText(QString::fromUtf8(QJsonDocument(proposal.fields).toJson()) + "\nLocal commit: " + proposal.commit + "\n\nComplete source patch:\n" + proposal.patch); layout.addWidget(&text);
        QPushButton approve("Approve completed draft and submit PR"); approve.setObjectName("approveCompletedPr"); layout.addWidget(&approve); connect(&approve, &QPushButton::clicked, &dialog, &QDialog::accept);
        QPushButton cancel("Cancel"); layout.addWidget(&cancel); connect(&cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
        if (dialog.exec() == QDialog::Accepted) submitProposal(proposal);
    });
    watcher->setFuture(QtConcurrent::run([file] { return LauncherTroubleshooting::reviewProposal(file); }));
}
void TroubleshootingDialog::submitProposal(const PrProposal &proposal) {
    if (!authenticated) return;
    const auto fields = proposal.fields;
    const auto head = fields.value("head").toString().split(':');
    const QString fork = head[0] + '/' + fields.value("repo").toString().section('/', 1);
    auto *check = new QProcess(this); status->setText("Verifying the published head matches the reviewed commit…");
    connect(check, &QProcess::finished, this, [this, check, proposal, fields](int code, QProcess::ExitStatus exit) {
        const QString published = QString::fromUtf8(check->readAllStandardOutput()).trimmed(); check->deleteLater();
        if (exit != QProcess::NormalExit || code != 0 || published != proposal.commit) { status->setText("Published head does not match the reviewed patch. Publish the reviewed branch separately after explicit approval, then review the proposal again. No PR submitted."); return; }
        auto *body = new QTemporaryFile; if (!body->open()) { delete body; status->setText("Cannot prepare approved PR body."); return; }
        body->write(fields.value("body").toString().toUtf8()); body->flush();
        auto *submit = new QProcess(this); review->setEnabled(false);
        connect(submit, &QProcess::finished, this, [this, submit, body](int code, QProcess::ExitStatus exit) { status->setText(exit == QProcess::NormalExit && code == 0 ? "PR submitted: " + QString::fromUtf8(submit->readAllStandardOutput()).trimmed() : "PR submission failed: " + QString::fromUtf8(submit->readAllStandardError()).trimmed()); submit->deleteLater(); delete body; review->setEnabled(authenticated); });
        connect(submit, &QProcess::errorOccurred, this, [this, submit, body](QProcess::ProcessError error) { if (error == QProcess::FailedToStart) { status->setText("Cannot start GitHub CLI PR submission."); review->setEnabled(authenticated); delete body; submit->deleteLater(); } });
        submit->start(gh, {"pr", "create", "--repo", fields.value("repo").toString(), "--base", fields.value("base").toString(), "--head", fields.value("head").toString(), "--title", fields.value("title").toString(), "--body-file", body->fileName()});
    });
    connect(check, &QProcess::errorOccurred, this, [this, check](QProcess::ProcessError error) { if (error == QProcess::FailedToStart) { status->setText("Cannot start GitHub CLI; no PR submitted."); check->deleteLater(); } });
    check->start(gh, {"api", "repos/" + fork + "/commits/" + QString::fromLatin1(QUrl::toPercentEncoding(head[1])), "--jq", ".sha"});
}
