// SPDX-License-Identifier: GPL-3.0-or-later
#include "prefixdialog.h"
#include "prefixbuilder.h"
#include "discovery.h"
#include "core.h"
#include "releases.h"
#include "log.h"
#include <QFutureWatcher>
#include <QtConcurrent>
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
    const QString source = LauncherDiscovery::managedSource(data);
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel("Downloads a prebuilt Darling runtime when the upstream publishes one (checked against its CI build attestation) and initializes a new prefix, or builds a private runtime from the darlinghq/darling source. Everything lives under " + data + "."); intro->setWordWrap(true); layout->addWidget(intro);
    auto *form = new QFormLayout;
    auto *sourceLabel = new QLabel(source); sourceLabel->setObjectName("prefixBuilderSource"); form->addRow("Source clone", sourceLabel);
    auto *workspaceLabel = new QLabel; workspaceLabel->setObjectName("prefixBuilderWorkspace"); form->addRow("New workspace", workspaceLabel);
    auto *jobs = new QSpinBox; jobs->setRange(1, 1024); jobs->setValue(1); form->addRow("Parallel jobs", jobs);
    auto *cmake = new QTextEdit; cmake->setPlaceholderText("Optional: one -DNAME=VALUE CMake setting per line"); cmake->setMaximumHeight(70); form->addRow("CMake settings", cmake);
    layout->addLayout(form);
    auto *status = new QLabel("Download is the quick way when available; darlinghq/darling publishes no prebuilt runtime yet. Building from source clones darlinghq/darling first and takes a long time; the settings above apply only to the build."); status->setObjectName("prefixBuilderStatus"); status->setWordWrap(true); layout->addWidget(status);
    auto *progress = new QProgressBar; progress->setRange(0, 1); progress->setValue(0); layout->addWidget(progress);
    auto *output = new QTextEdit; output->setReadOnly(true); layout->addWidget(output);
    auto *download = new QPushButton("Download prebuilt runtime"); download->setObjectName("downloadPrebuilt"); layout->addWidget(download);
    auto *build = new QPushButton("Build from source instead"); build->setObjectName("buildPrefix"); layout->addWidget(build);
    auto *background = new QPushButton("Continue in the background"); background->setObjectName("buildInBackground"); background->setEnabled(false); layout->addWidget(background);
    connect(background, &QPushButton::clicked, this, &QDialog::close);
    auto reopen = [this, background](bool success) { background->setEnabled(false); emit finished(); if (!(automatic && success)) { show(); raise(); activateWindow(); } };
    auto setBusy = [download, build](bool busy) { download->setEnabled(!busy); build->setEnabled(!busy); };
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
    connect(download, &QPushButton::clicked, this, [=] {
        downloading = true; setBusy(true); background->setEnabled(true); progress->setRange(0, 0);
        status->setText("Downloading the prebuilt runtime from the " + QString(LauncherReleases::repository) + " releases…");
        auto *watcher = new QFutureWatcher<LauncherCore::PrebuiltSetup>(this);
        connect(watcher, &QFutureWatcherBase::finished, this, [=] {
            const auto setup = watcher->result(); watcher->deleteLater(); downloading = false;
            setBusy(false); progress->setRange(0, 1); progress->setValue(setup.error.isEmpty());
            if (setup.error.isEmpty()) { status->setText("Prebuilt runtime and prefix are ready"); reopen(true); emit prefixReady(setup.prefix, setup.launcher, setup.runtimeRoot); return; }
            output->append(setup.error); LauncherLog::write("download", setup.error);
            // Only when nothing was installed (no gh, no release, unsupported host) is a source build a sensible fallback; a
            // runtime that installed but failed its prefix checks would very likely fail the same way after a long build.
            if (automatic && setup.launcher.isEmpty()) { output->append("\nNo usable prebuilt runtime; building from source instead.\n"); build->click(); return; }
            status->setText(setup.error + "\n\nYou can build from source instead."); reopen(false); emit failed(setup.error);
        });
        watcher->setFuture(QtConcurrent::run([gh = QStandardPaths::findExecutable("gh"), data] { return LauncherCore::setupPrebuilt(gh, LauncherReleases::repository, data); }));
    });
    auto startBuild = [=] {
        newWorkspace();
        const QString workspace = workspaceLabel->text();
        QString volume = QFileInfo(sourceVolume).canonicalFilePath();
        QString parent = QFileInfo(QFileInfo(workspace).absolutePath()).canonicalFilePath();
        if (!volume.isEmpty() && (parent == volume || parent.startsWith(volume + '/'))) { status->setText("The build workspace must be outside the macOS source volume."); return; }
        PrefixBuildRequest request{QStandardPaths::findExecutable("python3"), script, source, workspace, jobs->value(), {}};
        for (const auto &line : cmake->toPlainText().split('\n', Qt::SkipEmptyParts)) request.cmakeArguments << line.trimmed();
        QString message;
        if (!LauncherPrefix::validateRequest(request, &message)) { status->setText(message); return; }
        auto *builder = new PrefixBuilder(this);
        setBusy(true); background->setEnabled(true); progress->setRange(0, 0);
        connect(builder, &PrefixBuilder::output, output, &QTextEdit::insertPlainText);
        connect(builder, &PrefixBuilder::phaseChanged, status, &QLabel::setText);
        connect(builder, &PrefixBuilder::completed, this, [this, builder, setBusy, status, progress, reopen](bool success, const QString &message, const QString &prefix, const QString &launcher, const QString &runtime) {
            status->setText(message); setBusy(false); progress->setRange(0, 1); progress->setValue(success ? 1 : 0); builder->deleteLater(); reopen(success); if (!success) emit failed(message);
            if (success) emit prefixReady(prefix, launcher, runtime);
        });
        builder->start(request);
    };
    connect(build, &QPushButton::clicked, this, [=] {
        if (script.isEmpty() || !QFileInfo(script).isFile()) { status->setText("The bundled prefix builder could not be written under " + data + "/tools."); return; }
        QString git = QStandardPaths::findExecutable("git");
        if (git.isEmpty()) { status->setText("Install Git to clone darlinghq/darling."); return; }
        if (QFileInfo(source).isDir()) {
            QProcess origin; origin.start(git, {"-C", source, "remote", "get-url", "origin"});
            if (!origin.waitForFinished(5000) || origin.exitCode() != 0 || QString::fromUtf8(origin.readAllStandardOutput()).trimmed() != LauncherDiscovery::sourceUrl) {
                const QString message = "The clone in " + source + " does not have " + QString(LauncherDiscovery::sourceUrl) + " as origin (or git could not read it); move it aside and build again.";
                status->setText(message); background->setEnabled(false); reopen(false); emit failed(message); return;
            }
            auto *update = new QProcess(this); update->setProcessChannelMode(QProcess::MergedChannels);
            setBusy(true); background->setEnabled(true); progress->setRange(0, 0);
            status->setText("Updating the darlinghq/darling clone in " + source);
            connect(update, &QProcess::readyReadStandardOutput, this, [=] { output->insertPlainText(QString::fromUtf8(update->readAllStandardOutput())); });
            auto updated = [=](bool success) {
                setBusy(false); progress->setRange(0, 1); progress->setValue(success);
                if (success) startBuild(); else { status->setText("The darlinghq/darling clone could not be fast-forwarded to the current master. See output; resolve it in " + source + " and build again."); reopen(false); emit failed("The darlinghq/darling clone could not be updated."); }
                update->deleteLater();
            };
            connect(update, &QProcess::finished, this, [=](int code, QProcess::ExitStatus exit) { updated(code == 0 && exit == QProcess::NormalExit); });
            connect(update, &QProcess::errorOccurred, this, [=](QProcess::ProcessError error) { if (error == QProcess::FailedToStart) updated(false); });
            update->start(git, LauncherDiscovery::updateArguments(source));
            return;
        }
        if (!QDir().mkpath(QFileInfo(source).absolutePath())) { status->setText("Cannot create " + QFileInfo(source).absolutePath()); return; }
        auto *process = new QProcess(this); process->setProcessChannelMode(QProcess::MergedChannels);
        setBusy(true); background->setEnabled(true); progress->setRange(0, 0);
        status->setText("Cloning " + QString(LauncherDiscovery::sourceUrl) + " into " + source);
        connect(process, &QProcess::readyReadStandardOutput, this, [=] { output->insertPlainText(QString::fromUtf8(process->readAllStandardOutput())); });
        auto done = [=](bool success) {
            setBusy(false); progress->setRange(0, 1); progress->setValue(success);
            if (success) startBuild(); else { status->setText("Clone failed. See output; any partial clone is retained for inspection."); reopen(false); emit failed("The darlinghq/darling clone failed."); }
            process->deleteLater();
        };
        connect(process, &QProcess::finished, this, [=](int code, QProcess::ExitStatus exit) { done(code == 0 && exit == QProcess::NormalExit); });
        connect(process, &QProcess::errorOccurred, this, [=](QProcess::ProcessError error) { if (error == QProcess::FailedToStart) done(false); });
        process->start(git, LauncherDiscovery::cloneArguments(LauncherDiscovery::sourceUrl, source));
    });
}

void PrefixDialog::startAutomatically() {
    automatic = true;
    if (auto *download = findChild<QPushButton *>("downloadPrebuilt")) download->click();
}

PrefixDialog::~PrefixDialog() {
    for (auto *process : findChildren<QProcess *>(QString(), Qt::FindDirectChildrenOnly)) process->disconnect(this);
}

bool PrefixDialog::isBusy() const {
    if (downloading) return true;
    for (auto *builder : findChildren<PrefixBuilder *>()) if (builder->isRunning()) return true;
    for (auto *process : findChildren<QProcess *>(QString(), Qt::FindDirectChildrenOnly)) if (process->state() != QProcess::NotRunning) return true;
    return false;
}
void PrefixDialog::done(int result) { if (isBusy()) hide(); else QDialog::done(result); }
