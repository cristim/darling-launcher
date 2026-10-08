// SPDX-License-Identifier: GPL-3.0-or-later
#include "prefixdialog.h"
#include "prefixbuilder.h"
#include "discovery.h"
#include <QProcess>
#include <QCryptographicHash>
#include <QSaveFile>
#include <QDateTime>
#include <QSharedPointer>
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

PrefixDialog::PrefixDialog(const QString &sourceVolume, const QString &scriptOverride, QWidget *parent, bool managed) : QDialog(parent) {
    setObjectName("prefixBuildDialog");
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
    auto *build = new QPushButton("Build and deploy private runtime"); build->setObjectName("buildPrefix"); layout->addWidget(build);
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
    QString data = LauncherDiscovery::dataRoot();
    QDir().mkpath(data + "/workspaces");
    workspace->setText(LauncherDiscovery::newWorkspace(data + "/workspaces"));
    if (script->text().isEmpty() || !QFileInfo(script->text()).isFile() || (managed && scriptOverride.isEmpty())) {
        QFile bundled(":/launcher/tools/all-vibedarling-pr-prefix.py");
        if (bundled.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = bundled.readAll();
            const QString tool = data + "/tools/all-vibedarling-pr-prefix-" + QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) + ".py";
            if (QDir().mkpath(data + "/tools")) {
                if (!QFileInfo::exists(tool)) { QSaveFile file(tool); if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit()) script->setText(tool); }
                else { QFile file(tool); if (file.open(QIODevice::ReadOnly) && file.readAll() == bytes) script->setText(tool); }
            }
        }
    }
    if (managed) source->setText(data + "/sources/vibedarling");
    auto tools = LauncherDiscovery::scripts(LauncherDiscovery::roots());
    if ((script->text().isEmpty() || !QFileInfo(script->text()).isFile()) && !tools.isEmpty()) script->setText(tools.first());
    if (!managed && (source->text().isEmpty() || !QFileInfo(source->text()).isDir())) {
        QString detected = LauncherDiscovery::cleanSource(LauncherDiscovery::roots());
        source->setText(detected.isEmpty() ? data + "/sources/vibedarling" : detected);
    }
    auto *defaults = new QLabel("Sources, runtime images and prefixes are proposed under your launcher data folder. A missing source is cloned automatically when you choose Build. Existing paths remain configurable.");
    defaults->setWordWrap(true); layout->insertWidget(2, defaults);
    auto buildAfterClone = QSharedPointer<bool>::create(false);
    connect(clone, &QPushButton::clicked, this, [=] {
        QString destination = source->text();
        if (destination.isEmpty() || QFileInfo::exists(destination)) {
            QString parent = QFileDialog::getExistingDirectory(this, "Choose parent folder for a new VibeDarling clone", data);
            if (parent.isEmpty()) { *buildAfterClone = false; return; }
            destination = parent + "/vibedarling-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-hhmmsszzz");
        }
        if (!QDir::isAbsolutePath(destination) || !QDir().mkpath(QFileInfo(destination).absolutePath())) { *buildAfterClone = false; status->setText("Choose an absolute source path with a writable parent."); return; }
        if (QFileInfo::exists(destination)) { *buildAfterClone = false; status->setText("Clone destination already exists."); return; }
        QString git = QStandardPaths::findExecutable("git");
        if (git.isEmpty()) { *buildAfterClone = false; status->setText("Install Git to clone VibeDarling."); return; }
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
                if (*buildAfterClone) { *buildAfterClone = false; build->click(); }
            } else { *buildAfterClone = false; status->setText("Clone failed. See output; any partial clone is retained for inspection."); }
            process->deleteLater();
        };
        connect(process, &QProcess::finished, this, [=](int code, QProcess::ExitStatus exit) { done(code == 0 && exit == QProcess::NormalExit); });
        connect(process, &QProcess::errorOccurred, this, [=](QProcess::ProcessError error) { if (error == QProcess::FailedToStart) done(false); });
        process->start(git, LauncherDiscovery::cloneArguments("https://github.com/VibeDarling/darling.git", destination));
    });
    connect(build, &QPushButton::clicked, this, [=] {
        if (!QFileInfo(source->text()).isDir()) {
            if (!QFileInfo(script->text()).isFile()) { status->setText("Select a prefix-builder script before building."); return; }
            *buildAfterClone = true; clone->click(); return;
        }
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

bool PrefixDialog::isBusy() const {
    for (auto *builder : findChildren<PrefixBuilder *>()) if (builder->isRunning()) return true;
    for (auto *process : findChildren<QProcess *>(QString(), Qt::FindDirectChildrenOnly)) if (process->state() != QProcess::NotRunning) return true;
    return false;
}
void PrefixDialog::done(int result) { if (!isBusy()) QDialog::done(result); }
