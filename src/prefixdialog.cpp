// SPDX-License-Identifier: GPL-3.0-or-later
#include "prefixdialog.h"
#include "prefixbuilder.h"
#include "discovery.h"
#include <QProcess>
#include <QCryptographicHash>
#include <QSaveFile>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTextEdit>
#include <QVBoxLayout>

PrefixDialog::PrefixDialog(const QString &sourceVolume, const QString &scriptOverride, QWidget *parent) : QDialog(parent) {
    setObjectName("prefixBuildDialog");
    setWindowTitle("Set up Darling and a prefix"); resize(730, 560);
    const QString data = LauncherDiscovery::dataRoot();
    const QString source = data + "/sources/vibedarling";
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel("Clones VibeDarling, builds a private runtime and initializes a new prefix. Everything lives under " + data + "."); intro->setWordWrap(true); layout->addWidget(intro);
    auto *form = new QFormLayout;
    auto *sourceLabel = new QLabel(source); sourceLabel->setObjectName("prefixBuilderSource"); form->addRow("Source clone", sourceLabel);
    auto *workspaceLabel = new QLabel; workspaceLabel->setObjectName("prefixBuilderWorkspace"); form->addRow("New workspace", workspaceLabel);
    auto *scope = new QComboBox; scope->setObjectName("prefixBuilderScope"); scope->addItems({"Main/master (default branches) only", "Main/master plus all open PRs"}); form->addRow("Inputs", scope);
    auto *jobs = new QSpinBox; jobs->setRange(1, 1024); jobs->setValue(1); form->addRow("Parallel jobs", jobs);
    auto *cmake = new QTextEdit; cmake->setPlaceholderText("Optional: one -DNAME=VALUE CMake setting per line"); cmake->setMaximumHeight(70); form->addRow("CMake settings", cmake);
    layout->addLayout(form);
    auto *status = new QLabel("A missing source clone is created automatically when you build."); layout->addWidget(status);
    auto *progress = new QProgressBar; progress->setRange(0, 1); progress->setValue(0); layout->addWidget(progress);
    auto *output = new QTextEdit; output->setReadOnly(true); layout->addWidget(output);
    auto *build = new QPushButton("Build and deploy private runtime"); build->setObjectName("buildPrefix"); layout->addWidget(build);
    auto *background = new QPushButton("Continue in the background"); background->setObjectName("buildInBackground"); background->setEnabled(false); layout->addWidget(background);
    connect(background, &QPushButton::clicked, this, &QDialog::close);
    auto reopen = [this, background] { background->setEnabled(false); emit finished(); show(); raise(); activateWindow(); };
    auto newWorkspace = [workspaceLabel, data] { QDir().mkpath(data + "/workspaces"); workspaceLabel->setText(LauncherDiscovery::newWorkspace(data + "/workspaces")); };
    newWorkspace();
    QString script = scriptOverride;
    if (script.isEmpty()) {
        QFile bundled(":/launcher/tools/all-vibedarling-pr-prefix.py");
        if (bundled.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = bundled.readAll();
            const QString tool = data + "/tools/all-vibedarling-pr-prefix-" + QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) + ".py";
            if (QDir().mkpath(data + "/tools")) {
                if (!QFileInfo::exists(tool)) { QSaveFile file(tool); if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit()) script = tool; }
                else { QFile file(tool); if (file.open(QIODevice::ReadOnly) && file.readAll() == bytes) script = tool; }
            }
        }
    }
    auto startBuild = [=] {
        newWorkspace();
        const QString workspace = workspaceLabel->text();
        QString volume = QFileInfo(sourceVolume).canonicalFilePath();
        QString parent = QFileInfo(QFileInfo(workspace).absolutePath()).canonicalFilePath();
        if (!volume.isEmpty() && (parent == volume || parent.startsWith(volume + '/'))) { status->setText("The build workspace must be outside the macOS source volume."); return; }
        PrefixBuildRequest request{QStandardPaths::findExecutable("python3"), script, source, workspace, scope->currentIndex() == 1, jobs->value(), {}};
        for (const auto &line : cmake->toPlainText().split('\n', Qt::SkipEmptyParts)) request.cmakeArguments << line.trimmed();
        QString message;
        if (!LauncherPrefix::validateRequest(request, &message)) { status->setText(message); return; }
        auto *builder = new PrefixBuilder(this);
        build->setEnabled(false); background->setEnabled(true); progress->setRange(0, 0);
        connect(builder, &PrefixBuilder::output, output, &QTextEdit::insertPlainText);
        connect(builder, &PrefixBuilder::phaseChanged, status, &QLabel::setText);
        connect(builder, &PrefixBuilder::completed, this, [this, builder, build, status, progress, reopen](bool success, const QString &message, const QString &prefix, const QString &launcher, const QString &runtime) {
            status->setText(message); build->setEnabled(true); progress->setRange(0, 1); progress->setValue(success ? 1 : 0); builder->deleteLater(); reopen();
            if (success) emit prefixReady(prefix, launcher, runtime);
        });
        builder->start(request);
    };
    connect(build, &QPushButton::clicked, this, [=] {
        if (script.isEmpty() || !QFileInfo(script).isFile()) { status->setText("The bundled prefix builder could not be written under " + data + "/tools."); return; }
        if (QFileInfo(source).isDir()) { startBuild(); return; }
        QString git = QStandardPaths::findExecutable("git");
        if (git.isEmpty()) { status->setText("Install Git to clone VibeDarling."); return; }
        if (!QDir().mkpath(QFileInfo(source).absolutePath())) { status->setText("Cannot create " + QFileInfo(source).absolutePath()); return; }
        auto *process = new QProcess(this); process->setProcessChannelMode(QProcess::MergedChannels);
        build->setEnabled(false); background->setEnabled(true); progress->setRange(0, 0);
        status->setText("Cloning https://github.com/VibeDarling/darling.git into " + source);
        connect(process, &QProcess::readyReadStandardOutput, this, [=] { output->insertPlainText(QString::fromUtf8(process->readAllStandardOutput())); });
        auto done = [=](bool success) {
            build->setEnabled(true); progress->setRange(0, 1); progress->setValue(success);
            if (success) startBuild(); else { status->setText("Clone failed. See output; any partial clone is retained for inspection."); reopen(); }
            process->deleteLater();
        };
        connect(process, &QProcess::finished, this, [=](int code, QProcess::ExitStatus exit) { done(code == 0 && exit == QProcess::NormalExit); });
        connect(process, &QProcess::errorOccurred, this, [=](QProcess::ProcessError error) { if (error == QProcess::FailedToStart) done(false); });
        process->start(git, LauncherDiscovery::cloneArguments("https://github.com/VibeDarling/darling.git", source));
    });
}

PrefixDialog::~PrefixDialog() {
    for (auto *process : findChildren<QProcess *>(QString(), Qt::FindDirectChildrenOnly)) process->disconnect(this);
}

bool PrefixDialog::isBusy() const {
    for (auto *builder : findChildren<PrefixBuilder *>()) if (builder->isRunning()) return true;
    for (auto *process : findChildren<QProcess *>(QString(), Qt::FindDirectChildrenOnly)) if (process->state() != QProcess::NotRunning) return true;
    return false;
}
void PrefixDialog::done(int result) { if (isBusy()) hide(); else QDialog::done(result); }
