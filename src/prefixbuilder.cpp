// SPDX-License-Identifier: GPL-3.0-or-later
#include "prefixbuilder.h"
#include "log.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

namespace {
bool error(QString *target, const QString &message) { if (target) *target = message; return false; }
bool saveJson(const QString &path, const QJsonObject &object) {
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(object).toJson()) >= 0 && file.commit();
}
bool loadJson(const QString &path, QJsonObject *object) {
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return false;
    QJsonParseError parse; auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) return false;
    *object = document.object(); return true;
}
}
namespace LauncherPrefix {
bool selectInputs(const QJsonObject &discovery, bool includePrs, QJsonObject *selected, QString *message) {
    if (discovery.value("schema").toInt() != 1 || discovery.value("owner").toString() != "VibeDarling" || !discovery.value("complete").toBool())
        return error(message, "The builder did not produce a complete VibeDarling schema-1 lock");
    QJsonArray repos = discovery.value("repos").toArray();
    if (repos.isEmpty() || repos.first().toObject().value("repo").toString() != "darling")
        return error(message, "The lock has no Darling superproject");
    QJsonObject result = discovery; QJsonArray inputs;
    for (const auto &value : repos) {
        if (!value.isObject()) return error(message, "Invalid repository in discovery lock");
        auto item = value.toObject();
        if (!item.value("prs").isArray() || item.value("base").toString().isEmpty() || item.value("branch").toString().isEmpty())
            return error(message, "Discovery lock is missing repository inputs");
        if (!includePrs) item.insert("prs", QJsonArray{});
        inputs.append(item);
    }
    result.insert("repos", inputs);
    result.insert("launcher_input_selection", includePrs ? "default-branches-and-open-prs" : "default-branches-only");
    if (selected) *selected = result;
    return true;
}
bool selectNestedInputs(const QJsonObject &discovery, bool includePrs, QJsonObject *selected, QString *message) {
    if (discovery.value("schema").toInt() != 1 || discovery.value("owner").toString() != "VibeDarling" ||
        discovery.value("top_lock_sha256").toString().isEmpty() || !discovery.value("vibedarling").isArray() || !discovery.value("external_pinned").isArray())
        return error(message, "Invalid nested-submodule discovery lock");
    auto result = discovery; QJsonArray repos;
    for (const auto &value : discovery.value("vibedarling").toArray()) {
        auto item = value.toObject();
        if (!item.value("prs").isArray() || item.value("base").toString().isEmpty() || item.value("branch").toString().isEmpty())
            return error(message, "Nested lock is missing repository inputs");
        if (!includePrs) item.insert("prs", QJsonArray{});
        repos.append(item);
    }
    result.insert("vibedarling", repos);
    result.insert("launcher_input_selection", includePrs ? "default-branches-and-open-prs" : "default-branches-only");
    if (selected) *selected = result;
    return true;
}
bool validateRequest(const PrefixBuildRequest &request, QString *message) {
    if (!QDir::isAbsolutePath(request.python) || !QFileInfo(request.python).isExecutable()) return error(message, "Python 3 is unavailable");
    if (!QDir::isAbsolutePath(request.script) || !QFileInfo(request.script).isFile()) return error(message, "Select the prefix-builder script");
    if (!QDir::isAbsolutePath(request.source) || !QFileInfo(request.source).isDir()) return error(message, "Select a clean VibeDarling source clone");
    if (!QDir::isAbsolutePath(request.workspace) || QFileInfo::exists(request.workspace) || QFileInfo(request.workspace).isSymLink()) return error(message, "Choose a new, nonexistent workspace path");
    QString parent = QFileInfo(request.workspace).absolutePath();
    if (!QFileInfo(parent).isDir()) return error(message, "The workspace parent directory must exist");
    QString canonicalParent = QFileInfo(parent).canonicalFilePath(), source = QFileInfo(request.source).canonicalFilePath();
    if (canonicalParent == source || canonicalParent.startsWith(source + '/')) return error(message, "The new workspace must be outside the source checkout");
    if (request.jobs < 1) return error(message, "Build jobs must be positive");
    for (const auto &argument : request.cmakeArguments)
        if (!argument.startsWith("-D") || !argument.contains('=')) return error(message, "Use one -DNAME=VALUE CMake setting per line");
    return true;
}
}
PrefixBuilder::PrefixBuilder(QObject *parent) : QObject(parent) {
    process.setProcessChannelMode(QProcess::MergedChannels);
    connect(&process, &QProcess::readyReadStandardOutput, this, [this] {
        QString text = QString::fromLocal8Bit(process.readAllStandardOutput()); phaseOutput += text; LauncherLog::write("build", text); emit output(text);
    });
    connect(&process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        QString text = QString::fromLocal8Bit(process.readAllStandardOutput()); phaseOutput += text; LauncherLog::write("build", text); emit output(text);
        LauncherLog::write("build", "phase process exited with status " + QString::number(code));
        if (phase == Phase::Idle) return;
        if (code != 0 || status != QProcess::NormalExit) { fail("Prefix builder failed with exit status " + QString::number(code) + ". Inspect the output and retained workspace."); return; }
        advance();
    });
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) fail(process.errorString());
    });
}
void PrefixBuilder::start(const PrefixBuildRequest &value) {
    if (phase != Phase::Idle) return;
    QString message;
    if (!LauncherPrefix::validateRequest(value, &message)) { fail(message); return; }
    if (!staging.isValid()) { fail("Cannot create lock staging directory"); return; }
    request = value;
    QFile script(request.script); if (!script.open(QIODevice::ReadOnly)) { fail(script.errorString()); return; }
    QByteArray bytes = script.readAll(); scriptSnapshot = staging.path() + "/builder.py";
    QFile snapshot(scriptSnapshot);
    if (!snapshot.open(QIODevice::WriteOnly) || snapshot.write(bytes) != bytes.size()) { fail("Cannot snapshot the selected builder script"); return; }
    snapshot.close();
    provenance = {{"script", request.script}, {"script_sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())},
                  {"source_clone", request.source}, {"workspace", request.workspace}, {"include_open_prs", request.includePrs}, {"jobs", request.jobs}, {"cmake_arguments", QJsonArray::fromStringList(request.cmakeArguments)}};
    run(Phase::Inspect, {"--help"});
}
void PrefixBuilder::run(Phase next, const QStringList &arguments) {
    phase = next; phaseOutput.clear();
    QString label;
    switch (next) {
    case Phase::Inspect: label = "Check prefix-builder interface"; break;
    case Phase::Resolve: label = "Resolve current branches and PRs"; break;
    case Phase::Checkout: label = "Create independent checkout"; break;
    case Phase::ResolveNested: label = "Resolve nested submodule inputs"; break;
    case Phase::CheckoutNested: label = "Integrate nested submodules"; break;
    case Phase::Build: label = "Wait for shared build lock, then build private Darling runtime"; break;
    case Phase::Idle: return;
    }
    emit phaseChanged(label);
    process.setWorkingDirectory(staging.path());
    QStringList args{"-u", scriptSnapshot}; args.append(arguments);
    if (next == Phase::Build) {
        const QString flock = QStandardPaths::findExecutable("flock");
        if (flock.isEmpty() || !QDir().mkpath("/tmp/agent-locks")) { fail("Cannot acquire the shared Darling heavy-build lock; install flock and check the lock directory."); return; }
        process.start(flock, QStringList{"-w", "600", "/tmp/agent-locks/darling-heavy-build.lock", request.python} + args);
    } else process.start(request.python, args);
}
void PrefixBuilder::advance() {
    if (phase == Phase::Inspect) {
        if (!phaseOutput.contains("resolve") || !phaseOutput.contains("checkout") || !phaseOutput.contains("build")) { fail("The selected script does not expose the expected builder interface"); return; }
        supportsNested = phaseOutput.contains("resolve-nested") && phaseOutput.contains("checkout-nested");
        provenance.insert("nested_submodule_workflow", supportsNested);
        run(Phase::Resolve, {"resolve", "--source", request.source, "--output", staging.path() + "/discovery.lock.json", "--jobs", QString::number(request.jobs)});
    } else if (phase == Phase::Resolve) {
        QJsonObject discovery, selected; QString message;
        if (!loadJson(staging.path() + "/discovery.lock.json", &discovery)) { fail("Cannot read the discovery lock"); return; }
        if (!LauncherPrefix::selectInputs(discovery, request.includePrs, &selected, &message)) { fail(message); return; }
        if (!saveJson(staging.path() + "/selected.lock.json", selected)) { fail("Cannot write selected input lock"); return; }
        run(Phase::Checkout, {"checkout", "--lock", staging.path() + "/selected.lock.json", "--workspace", request.workspace, "--jobs", QString::number(request.jobs)});
    } else if (phase == Phase::Checkout) {
        if (!QFile::copy(staging.path() + "/discovery.lock.json", request.workspace + "/refs.discovery.lock.json") ||
            !saveJson(request.workspace + "/launcher-build-request.json", provenance)) { fail("Cannot retain discovery provenance in the new workspace"); return; }
        if (supportsNested) run(Phase::ResolveNested, {"resolve-nested", "--workspace", request.workspace, "--output", staging.path() + "/nested.discovery.lock.json", "--jobs", QString::number(request.jobs)});
        else build();
    } else if (phase == Phase::ResolveNested) {
        QJsonObject discovery, selected; QString message;
        if (!loadJson(staging.path() + "/nested.discovery.lock.json", &discovery)) { fail("Cannot read nested discovery lock"); return; }
        if (!LauncherPrefix::selectNestedInputs(discovery, request.includePrs, &selected, &message)) { fail(message); return; }
        if (!saveJson(staging.path() + "/nested.selected.lock.json", selected)) { fail("Cannot write nested input selection"); return; }
        run(Phase::CheckoutNested, {"checkout-nested", "--workspace", request.workspace, "--lock", staging.path() + "/nested.selected.lock.json"});
    } else if (phase == Phase::CheckoutNested) {
        if (!QFile::copy(staging.path() + "/nested.discovery.lock.json", request.workspace + "/nested.discovery.lock.json")) { fail("Cannot preserve nested discovery inputs"); return; }
        build();
    } else if (phase == Phase::Build) {
        QString prefix = request.workspace + "/prefix", launcher = request.workspace + "/build/src/startup/darling", runtime = request.workspace + "/image/usr/local";
        if (!QFileInfo::exists(prefix + "/private/etc/passwd") || !QFileInfo(launcher).isExecutable() || !QFileInfo(runtime).isDir()) {
            fail("Builder exited successfully but the prefix, executable or runtime image is missing"); return;
        }
        provenance.insert("input_lock", request.workspace + "/refs.lock.json");
        provenance.insert("discovery_lock", request.workspace + "/refs.discovery.lock.json");
        provenance.insert("integrated_manifest", request.workspace + "/integrated.json");
        QStringList manifests{"refs.lock.json", "integrated.json"};
        if (supportsNested) manifests << "nested.refs.lock.json" << "nested.integrated.json";
        for (const auto &name : manifests) {
            QFile file(request.workspace + '/' + name);
            if (!file.open(QIODevice::ReadOnly)) { fail("Cannot verify build input provenance: " + name); return; }
            provenance.insert(name + "_sha256", QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex()));
        }
        provenance.insert("completed_at_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        provenance.insert("launcher", launcher); provenance.insert("runtime_install_root", runtime);
        QDir().mkpath(prefix + "/.darling-launcher");
        if (!saveJson(prefix + "/.darling-launcher/build-provenance.json", provenance)) { fail("Cannot record prefix build provenance"); return; }
        phase = Phase::Idle; emit completed(true, "Prefix and runtime are ready", prefix, launcher, runtime);
    }
}
void PrefixBuilder::build() {
    QStringList args{"build", "--workspace", request.workspace, "--jobs", QString::number(request.jobs)};
    for (const auto &argument : request.cmakeArguments) args << "--cmake-arg=" + argument;
    run(Phase::Build, args);
}
void PrefixBuilder::fail(const QString &message) { phase = Phase::Idle; emit completed(false, message, {}, {}, {}); }
PrefixBuilder::~PrefixBuilder() { process.disconnect(this); }

bool PrefixBuilder::isRunning() const { return phase != Phase::Idle; }
