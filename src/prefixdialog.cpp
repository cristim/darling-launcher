#include "prefixdialog.h"
#include "prefixbuilder.h"
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
