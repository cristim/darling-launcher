// SPDX-License-Identifier: GPL-3.0-or-later
#include "prefixdialog.h"
#include "prefixbuilder.h"
#include "discovery.h"
#include <QProcess>
#include <QDateTime>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTextEdit>
#include <QVBoxLayout>

PrefixDialog::PrefixDialog(const QString &sourceVolume, const QString &scriptOverride, QWidget *parent) : QDialog(parent) {
    setWindowTitle("Build an isolated VibeDarling prefix"); resize(730, 560);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel("Use all-vibedarling-pr-prefix.py to resolve inputs, clone independently,\nbuild a private runtime image and initialize a new prefix."));
    auto *form = new QFormLayout;
    auto field = [&](const QString &label, bool file) {
        auto *row = new QHBoxLayout; auto *edit = new QLineEdit; auto *browse = new QPushButton("Browse…"); row->addWidget(edit); row->addWidget(browse);
        connect(browse, &QPushButton::clicked, this, [this, edit, file] {
            QString path = file ? QFileDialog::getOpenFileName(this, "Choose prefix-builder script", {}, "Python scripts (*.py)") : QFileDialog::getExistingDirectory(this, "Choose clean source clone");
            if (!path.isEmpty()) edit->setText(path);
        });
        form->addRow(label, row); return edit;
    };
    auto *script = field("Prefix-builder script", true); script->setObjectName("prefixBuilderScript");
    auto *source = field("Clean VibeDarling source clone", false); source->setObjectName("prefixBuilderSource");
    auto *workspace = new QLineEdit; workspace->setObjectName("prefixBuilderWorkspace"); form->addRow("New workspace (must not exist)", workspace);
    auto *scope = new QComboBox; scope->setObjectName("prefixBuilderScope"); scope->addItems({"Main/master (default branches) only", "Main/master plus all open PRs"}); form->addRow("Inputs", scope);
    auto *jobs = new QSpinBox; jobs->setRange(1, 1024); jobs->setValue(1); form->addRow("Parallel jobs", jobs);
    auto *cmake = new QTextEdit; cmake->setPlaceholderText("Optional: one -DNAME=VALUE CMake setting per line"); cmake->setMaximumHeight(70); form->addRow("CMake settings", cmake);
    layout->addLayout(form);
    auto *destination = new QLabel("Prefix output: choose a workspace"); layout->addWidget(destination);
    connect(workspace, &QLineEdit::textChanged, destination, [destination](const QString &path) { destination->setText("Prefix output: " + path + "/prefix\nRuntime output: " + path + "/image/usr/local"); });
    auto *status = new QLabel("The source clone is read-only input. Existing workspaces are refused."); layout->addWidget(status);
    auto *progress = new QProgressBar; progress->setRange(0, 1); progress->setValue(0); layout->addWidget(progress);
    auto *output = new QTextEdit; output->setReadOnly(true); layout->addWidget(output);
    auto *build = new QPushButton("Build prefix"); build->setObjectName("buildPrefix"); layout->addWidget(build);
    QSettings settings("cristim", "darling-launcher");
    script->setText(scriptOverride.isEmpty() ? settings.value("builderScript").toString() : scriptOverride);
    source->setText(settings.value("builderSource").toString());
    auto *setup = new QHBoxLayout;
    auto *detect = new QPushButton("Find local tools and sources");
    auto *clone = new QPushButton("Clone VibeDarling…");
    setup->addWidget(detect); setup->addWidget(clone); layout->insertLayout(1, setup);
    auto choose = [this](const QStringList &paths, const QString &title) -> QString {
        if (paths.isEmpty()) return {};
        if (paths.size() == 1) return paths.first();
        QDialog dialog(this); dialog.setWindowTitle(title); QVBoxLayout box(&dialog);
        QComboBox options; options.addItems(paths); box.addWidget(&options);
        QPushButton select("Use selected path"); box.addWidget(&select);
        connect(&select, &QPushButton::clicked, &dialog, &QDialog::accept);
        return dialog.exec() == QDialog::Accepted ? options.currentText() : QString();
    };
    connect(detect, &QPushButton::clicked, this, [=] {
        auto roots = LauncherDiscovery::roots();
        QString tool = choose(LauncherDiscovery::scripts(roots), "Select detected prefix builder");
        QString checkout = choose(LauncherDiscovery::sources(roots), "Select detected source checkout");
        if (!tool.isEmpty()) script->setText(tool);
        if (!checkout.isEmpty()) source->setText(checkout);
        status->setText("Detected paths selected. The builder checks that the source is clean and uses VibeDarling refs.");
    });
    QString data = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(data + "/workspaces");
    workspace->setText(LauncherDiscovery::newWorkspace(data + "/workspaces"));
    auto tools = LauncherDiscovery::scripts(LauncherDiscovery::roots());
    if (script->text().isEmpty() && tools.size() == 1) script->setText(tools.first());
    connect(clone, &QPushButton::clicked, this, [=] {
        QString parent = QFileDialog::getExistingDirectory(this, "Choose parent folder for a new VibeDarling clone", data);
        if (parent.isEmpty()) return;
        QString destination = parent + "/vibedarling-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-hhmmsszzz");
        if (QFileInfo::exists(destination)) { status->setText("Clone destination already exists."); return; }
        QString git = QStandardPaths::findExecutable("git");
        if (git.isEmpty()) { status->setText("Install Git to clone VibeDarling."); return; }
        auto *process = new QProcess(this); process->setProcessChannelMode(QProcess::MergedChannels);
        clone->setEnabled(false); build->setEnabled(false); progress->setRange(0,0);
        status->setText("Cloning https://github.com/VibeDarling/darling.git into " + destination);
        connect(process, &QProcess::readyReadStandardOutput, this, [=] { output->insertPlainText(QString::fromUtf8(process->readAllStandardOutput())); });
        auto done = [=](bool success) {
            clone->setEnabled(true); build->setEnabled(true); progress->setRange(0,1); progress->setValue(success);
            if (success) {
                source->setText(destination);
                QString tool = destination + "/tools/all-vibedarling-pr-prefix.py";
                if (QFileInfo(tool).isFile()) script->setText(tool);
                QSettings("cristim", "darling-launcher").setValue("builderSource", destination);
                status->setText("Independent VibeDarling clone ready. Choose inputs and Build prefix.");
            } else status->setText("Clone failed. See output; any partial clone is retained for inspection.");
            process->deleteLater();
        };
        connect(process, &QProcess::finished, this, [=](int code, QProcess::ExitStatus exit) { done(code == 0 && exit == QProcess::NormalExit); });
        connect(process, &QProcess::errorOccurred, this, [=](QProcess::ProcessError error) { if (error == QProcess::FailedToStart) done(false); });
        process->start(git, LauncherDiscovery::cloneArguments("https://github.com/VibeDarling/darling.git", destination));
    });
    connect(build, &QPushButton::clicked, this, [=] {
        QString volume = QFileInfo(sourceVolume).canonicalFilePath();
        QString parent = QFileInfo(QFileInfo(workspace->text()).absolutePath()).canonicalFilePath();
        if (!volume.isEmpty() && (parent == volume || parent.startsWith(volume + '/'))) { status->setText("The build workspace must be outside the macOS source volume."); return; }
        PrefixBuildRequest request{QStandardPaths::findExecutable("python3"), script->text(), source->text(), workspace->text(), scope->currentIndex() == 1, jobs->value(), {}};
        for (const auto &line : cmake->toPlainText().split('\n', Qt::SkipEmptyParts)) request.cmakeArguments << line.trimmed();
        QString message;
        if (!LauncherPrefix::validateRequest(request, &message)) { status->setText(message); return; }
        QSettings settings("cristim", "darling-launcher"); settings.setValue("builderScript", request.script); settings.setValue("builderSource", request.source);
        auto *builder = new PrefixBuilder(this);
        build->setEnabled(false); progress->setRange(0, 0);
        connect(builder, &PrefixBuilder::output, output, &QTextEdit::insertPlainText);
        connect(builder, &PrefixBuilder::phaseChanged, status, &QLabel::setText);
        connect(builder, &PrefixBuilder::completed, this, [this, builder, build, status, progress](bool success, const QString &message, const QString &prefix, const QString &launcher, const QString &runtime) {
            status->setText(message); build->setEnabled(true); progress->setRange(0, 1); progress->setValue(success ? 1 : 0); builder->deleteLater();
            if (success) emit prefixReady(prefix, launcher, runtime);
        });
        builder->start(request);
    });
}

PrefixDialog::~PrefixDialog() {
    for (auto *process : findChildren<QProcess *>(QString(), Qt::FindDirectChildrenOnly)) process->disconnect(this);
}
