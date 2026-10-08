// SPDX-License-Identifier: GPL-3.0-or-later
#include "troubleshooting.h"
#include "core.h"
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
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrent>
namespace {
QString query(const QString &program, const QStringList &args, bool *ok = nullptr) {
    QProcess process; process.start(program, args);
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
PrProposal LauncherTroubleshooting::reviewProposal(const QString &file) {
    PrProposal result; QFile input(file);
    if (!input.open(QIODevice::ReadOnly)) { result.error = input.errorString(); return result; }
    result.fields = QJsonDocument::fromJson(input.readAll()).object();
    for (const QString &field : {"source", "repo", "base", "head", "title", "body"})
        if (result.fields.value(field).toString().trimmed().isEmpty()) { result.error = "Completed proposal requires source, repo, base, head, title and body."; return result; }
    const auto fields = result.fields;
    const QString source = QFileInfo(fields.value("source").toString()).canonicalFilePath();
    const QString workspace = QFileInfo(file).absolutePath();
    if (source.isEmpty() || !source.startsWith(QFileInfo(workspace).canonicalFilePath() + '/') || !QFileInfo(source + "/.git").isDir() || QFileInfo(source + "/.git").isSymLink()) { result.error = "Source must be an independent clone inside this proposal's workspace."; return result; }
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
    const QString range = fields.value("base").toString() + "...HEAD";
    const QString numbers = run({"diff", "--numstat", range});
    if (!ok || numbers.isEmpty() || QRegularExpression("(^|\\n)-\\t").match(numbers).hasMatch()) { result.error = "Proposal must contain source changes and no binary patches."; return result; }
    result.patch = run({"diff", range});
    if (!ok || result.patch.isEmpty()) result.error = "Cannot read the complete proposed source patch.";
    result.fields.insert("source", source);
    return result;
}
TroubleshootingDialog::TroubleshootingDialog(const QJsonObject &diagnostic, QWidget *parent) : QDialog(parent) {
    setObjectName("troubleshootingDialog"); setWindowTitle("Troubleshoot app failure"); resize(780, 600);
    auto *layout = new QVBoxLayout(this);
    auto *choices = new QLabel("Local workaround: import the exact missing library into this private prefix and retry from the launcher.\nIssue draft: save reproduction steps and dependency provenance for your review; saving does not submit it.\nSource fix: start an installed agent in a separate workspace using the troubleshooting data shown below."); choices->setObjectName("contributionChoices"); choices->setWordWrap(true); layout->addWidget(choices);
    auto *saveIssue = new QPushButton("Save local issue draft…"); saveIssue->setObjectName("saveFailureIssue"); layout->addWidget(saveIssue);
    connect(saveIssue, &QPushButton::clicked, this, [this, diagnostic] {
        const QString file = QFileDialog::getSaveFileName(this, "Save proposed issue", "proposed-vibedarling-issue.md", "Markdown (*.md)");
        if (file.isEmpty()) return;
        QSaveFile output(file);
        const AppEntry app{diagnostic.value("app").toString(), diagnostic.value("bundle").toString(), diagnostic.value("executable").toString(), diagnostic.value("sourceBundle").toString()};
        const QString body = LauncherCore::issueDraft(app, diagnostic.value("dependencyChain").toArray(), diagnostic.value("loaderOutput").toString(), diagnostic.value("sourceVolume").toString(), diagnostic.value("prefix").toString(), diagnostic.value("launcher").toString(), diagnostic.value("runtime").toString());
        if (!output.open(QIODevice::WriteOnly) || output.write(body.toUtf8()) < 0 || !output.commit()) QMessageBox::warning(this, "Issue draft", "Cannot save the local draft.");
    });
    auto *notice = new QLabel("Choose an installed agent for a separate clean-room source fix. Starting it shares loader output and local path provenance with that tool’s configured service. No Apple payloads are included. Work stays in a new workspace; no issue, push or PR is automatic."); notice->setWordWrap(true); layout->addWidget(notice);
    auto *details = new QTextEdit; details->setReadOnly(true); details->setObjectName("troubleshootingData"); details->setPlainText(QString::fromUtf8(QJsonDocument(diagnostic).toJson())); layout->addWidget(details);
    auto *agents = new QComboBox; agents->setObjectName("agentChoices"); agents->addItems(LauncherTroubleshooting::agents()); layout->addWidget(agents);
    auto *workspace = new QLineEdit(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/source-fixes/" + QUuid::createUuid().toString(QUuid::WithoutBraces)); workspace->setObjectName("agentWorkspace"); layout->addWidget(new QLabel("New source-fix workspace")); layout->addWidget(workspace);
    auto *consent = new QCheckBox("I approve sharing the displayed troubleshooting data with the selected agent’s configured service."); consent->setObjectName("approveAgentData"); layout->addWidget(consent);
    auto *start = new QPushButton("Start selected agent"); start->setObjectName("startFixAgent"); start->setEnabled(false);
    connect(consent, &QCheckBox::toggled, this, [=](bool approved) { start->setEnabled(approved && agents->count() > 0 && !QStandardPaths::findExecutable("xdg-terminal-exec").isEmpty()); }); layout->addWidget(start);
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
        const QString instructions = "Use TROUBLESHOOTING.json to investigate this Darling app failure. Clone relevant VibeDarling source repositories independently inside this workspace; do not modify existing checkouts or shared runtimes. Read their AGENTS.md. Work only from source, published APIs, headers, interface metadata and loader output. Never disassemble, decompile, inspect machine code, dump Apple symbols or commit Apple apps/libraries. Do not inspect dyld cache implementation bytes. Use private prefixes; do not unlock or change encrypted volumes. Hold /tmp/agent-locks/darling-heavy-build.lock for heavy builds. Prepare a source fix with meaningful tests and a local completed PR draft; no push, issue or PR submission is authorized. If a source fix cannot be specified without binary inspection, explain the blocker. Write proposal.json beside this report with string fields source (absolute independent clone path), repo (VibeDarling/repo), base (explicit branch), head (fork-owner:branch), title, body. Commit the reviewed source patch on that head branch. A branch must be published separately with explicit user approval before the launcher's PR submission can succeed.";
        QSaveFile prompt(directory + "/FIX-INSTRUCTIONS.txt");
        if (!prompt.open(QIODevice::WriteOnly) || prompt.write(instructions.toUtf8()) < 0 || !prompt.commit()) { status->setText("Cannot save agent instructions."); return; }
        const QString chosen = agents->currentText(), executable = QStandardPaths::findExecutable(chosen);
        const auto arguments = LauncherTroubleshooting::agentArguments(chosen, instructions);
        if (executable.isEmpty() || arguments.isEmpty()) { status->setText("Selected agent is no longer installed."); return; }
        const bool launched = QProcess::startDetached(QStandardPaths::findExecutable("xdg-terminal-exec"), QStringList{"--dir=" + directory, "--", executable} + arguments, directory);
        status->setText(launched ? "Agent started in " + directory + ". Review its patch and completed draft before publishing anything." : "Could not open the selected agent in a terminal.");
    });
    review = new QPushButton("Review completed PR proposal…"); review->setObjectName("reviewPrProposal"); review->setEnabled(false); layout->addWidget(review);
    connect(review, &QPushButton::clicked, this, [this] { const QString file = QFileDialog::getOpenFileName(this, "Choose completed proposal.json", {}, "JSON (*.json)"); if (!file.isEmpty()) loadProposal(file); });
    gh = QStandardPaths::findExecutable("gh");
    if (gh.isEmpty()) status->setText("GitHub CLI is not installed. Agent work and local drafts are available.");
    else {
        auto *check = new QProcess(this);
        connect(check, &QProcess::finished, this, [this, check](int code, QProcess::ExitStatus exit) { authenticated = code == 0 && exit == QProcess::NormalExit; review->setEnabled(authenticated); status->setText(authenticated ? "GitHub CLI authenticated. Completed PR drafts can be reviewed and explicitly approved here." : "GitHub CLI is not authenticated. Sign in separately before submitting a reviewed PR."); check->deleteLater(); });
        connect(check, &QProcess::errorOccurred, this, [this, check](QProcess::ProcessError error) { if (error == QProcess::FailedToStart) { status->setText("Cannot start GitHub CLI authentication check."); check->deleteLater(); } });
        QTimer::singleShot(15000, check, [this, check] { if (check->state() != QProcess::NotRunning) { status->setText("GitHub CLI authentication check timed out."); check->kill(); } });
        check->start(gh, {"auth", "status", "--hostname", "github.com"});
    }
    auto *close = new QPushButton("Close"); layout->addWidget(close); connect(close, &QPushButton::clicked, this, &QDialog::reject);
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
