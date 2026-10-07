#include "core.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStorageInfo>
#include <plist/plist.h>
#include <cstdlib>

namespace {
bool fail(QString *error, const QString &message) { if (error) *error = message; return false; }

bool confinedRelative(const QString &path) {
    if (path.isEmpty() || QDir::isAbsolutePath(path)) return false;
    for (const auto &part : path.split('/'))
        if (part.isEmpty() || part == "." || part == "..") return false;
    return true;
}

bool within(const QString &child, const QString &root) {
    return child == root || child.startsWith(root + '/');
}

bool copyTree(const QString &source, const QString &destination, const QString &sourceRoot, QString *error) {
    QFileInfo info(source);
    if (info.isSymLink()) {
        const QString target = info.symLinkTarget();
        if (!within(QFileInfo(target).canonicalFilePath(), sourceRoot))
            return fail(error, "Symlink leaves the selected bundle: " + source);
        QString raw = QDir(QFileInfo(source).absolutePath()).relativeFilePath(target);
        if (!QFile::link(raw, destination)) return fail(error, "Cannot copy symlink: " + destination);
        return true;
    }
    if (info.isDir()) {
        if (!QDir().mkpath(destination)) return fail(error, "Cannot create: " + destination);
        QDir dir(source);
        for (const QFileInfo &child : dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
            if (!copyTree(child.absoluteFilePath(), destination + '/' + child.fileName(), sourceRoot, error)) return false;
        return true;
    }
    if (!info.isFile() || !QFile::copy(source, destination)) return fail(error, "Cannot copy: " + source);
    QFile::setPermissions(destination, info.permissions());
    return true;
}

bool safeCopy(const QString &source, const QString &destination, const QString &volume, const QString &prefix, QString *error) {
    const QString sourceRoot = QFileInfo(source).canonicalFilePath();
    const QString mounted = QFileInfo(volume).canonicalFilePath();
    if (sourceRoot.isEmpty() || !within(sourceRoot, mounted) || sourceRoot == mounted)
        return fail(error, "Source is outside the selected volume");
    const QString root = QFileInfo(prefix).canonicalFilePath();
    const QString relativeParent = QDir(prefix).relativeFilePath(QFileInfo(destination).absolutePath());
    if (relativeParent.startsWith("..") || QDir::isAbsolutePath(relativeParent))
        return fail(error, "Destination leaves the selected prefix");
    QString parent = prefix;
    for (const QString &part : relativeParent.split('/', Qt::SkipEmptyParts)) {
        if (part == ".") continue;
        parent += '/' + part;
        if (QFileInfo(parent).isSymLink() || !QDir().mkpath(parent) || !within(QFileInfo(parent).canonicalFilePath(), root))
            return fail(error, "Destination parent is unsafe: " + parent);
    }
    if (QFileInfo::exists(destination) || QFileInfo(destination).isSymLink())
        return fail(error, "Destination already exists: " + destination);
    if (!copyTree(source, destination, sourceRoot, error)) {
        QFileInfo(destination).isDir() ? QDir(destination).removeRecursively() : QFile::remove(destination);
        return false;
    }
    return true;
}

QString plistString(const QString &path, const char *key) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QByteArray bytes = file.readAll();
    plist_t root = nullptr;
    if (plist_from_memory(bytes.constData(), uint32_t(bytes.size()), &root, nullptr) != PLIST_ERR_SUCCESS || !root) return {};
    char *value = nullptr;
    plist_t item = plist_dict_get_item(root, key);
    if (item && plist_get_node_type(item) == PLIST_STRING) plist_get_string_val(item, &value);
    QString result = value ? QString::fromUtf8(value) : QString();
    free(value);
    plist_free(root);
    return result;
}
}

namespace LauncherCore {
bool looksLikeMacVolume(const QString &root) {
    return QDir(root + "/System/Library").exists() &&
           (QDir(root + "/Applications").exists() || QDir(root + "/System/Applications").exists());
}
QStringList mountedMacVolumes() {
    QStringList result;
    for (const QStorageInfo &storage : QStorageInfo::mountedVolumes()) {
        if (!storage.isValid() || !storage.isReady()) continue;
        QString root = storage.rootPath();
        if (root == "/" || root.isEmpty()) continue;
        if (looksLikeMacVolume(root)) result << root;
    }
    result.removeDuplicates(); result.sort();
    return result;
}
QStringList discoverApps(const QString &volume) {
    QStringList found;
    for (const QString &base : {"Applications", "System/Applications"}) {
        QDir dir(volume + '/' + base);
        for (const QFileInfo &item : dir.entryInfoList({"*.app"}, QDir::Dirs | QDir::NoDotAndDotDot))
            if (!item.isSymLink()) found << base + '/' + item.fileName();
    }
    found.sort(Qt::CaseInsensitive);
    return found;
}

bool validateLocations(const QString &volume, const QString &prefix, QString *error) {
    QString v = QFileInfo(volume).canonicalFilePath();
    QString p = QFileInfo(prefix).canonicalFilePath();
    if (v.isEmpty() || !QFileInfo(v).isDir() || v == "/") return fail(error, "Select a mounted macOS volume directory");
    if (p.isEmpty() || !QFileInfo(p).isDir() || p == "/") return fail(error, "Select an existing prefix directory");
    if (within(p, v) || within(v, p)) return fail(error, "Volume and prefix must be separate");
    return true;
}

bool importApp(const QString &volume, const QString &prefix, const QString &relativeBundle, AppEntry *result, QString *error) {
    if (!validateLocations(volume, prefix, error)) return false;
    if (!confinedRelative(relativeBundle) || !relativeBundle.endsWith(".app") || !discoverApps(volume).contains(relativeBundle))
        return fail(error, "Select an app discovered on the volume");
    const QString source = volume + '/' + relativeBundle;
    QString executable = plistString(source + "/Contents/Info.plist", "CFBundleExecutable");
    if (executable.isEmpty() || !confinedRelative(executable) || executable.contains('/'))
        return fail(error, "Bundle has no valid CFBundleExecutable");
    if (!QFileInfo::exists(source + "/Contents/MacOS/" + executable))
        return fail(error, "Bundle executable is missing");
    const QString destRelative = "Applications/" + QFileInfo(source).fileName();
    if (!safeCopy(source, prefix + '/' + destRelative, volume, prefix, error)) return false;
    if (result) *result = {QFileInfo(source).completeBaseName(), destRelative, executable, relativeBundle};
    return true;
}

bool importLibrary(const QString &volume, const QString &prefix, const QString &expectedIn, QString *error) {
    if (!validateLocations(volume, prefix, error)) return false;
    if (!expectedIn.startsWith('/') || !confinedRelative(expectedIn.mid(1)))
        return fail(error, "Loader library path is not absolute and confined");
    QString relative = expectedIn.mid(1);
    const QRegularExpression framework(R"((^|/)([^/]+\.framework)(/|$))");
    auto match = framework.match(relative);
    if (match.hasMatch()) relative = relative.left(match.capturedEnd(2));
    if (!relative.startsWith("System/Library/") && !relative.startsWith("usr/lib/") && !relative.startsWith("Library/Frameworks/"))
        return fail(error, "Library path is outside supported macOS system locations");
    return safeCopy(volume + '/' + relative, prefix + '/' + relative, volume, prefix, error);
}

MissingSymbol diagnose(const QString &output) {
    QRegularExpression expression(R"(Symbol not found:\s*([^\s]+)\s+Referenced from:\s*([^\n]+)\n\s*Expected in:\s*(/[^\s]+))");
    auto match = expression.match(output);
    if (!match.hasMatch()) return {};
    return {match.captured(1), match.captured(2).trimmed(), match.captured(3)};
}

QString catalogPath(const QString &prefix) { return prefix + "/.darling-launcher/catalog.json"; }
QJsonObject loadCatalog(const QString &prefix) {
    QFile file(catalogPath(prefix));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
bool saveCatalog(const QString &prefix, const QJsonObject &catalog, QString *error) {
    QString path = catalogPath(prefix);
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return fail(error, "Cannot create catalog directory");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return fail(error, file.errorString());
    file.write(QJsonDocument(catalog).toJson());
    if (!file.commit()) return fail(error, file.errorString());
    return true;
}
bool stageBrewfile(const QString &prefix, const QString &source, QString *guestPath, QString *error) {
    if (!QFileInfo(prefix).isDir()) return fail(error, "Prefix does not exist");
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly)) return fail(error, input.errorString());
    QByteArray data = input.readAll();
    if (QRegularExpression(R"((?m)^\s*mas(?:\s|\())").match(QString::fromUtf8(data)).hasMatch())
        return fail(error, "MAS entries need a separate Apple ID workflow");
    QString directory = prefix + "/.darling-launcher";
    if (QFileInfo(directory).isSymLink() || !QDir().mkpath(directory))
        return fail(error, "Cannot create a safe Brewfile staging directory");
    QSaveFile target(directory + "/Brewfile");
    if (!target.open(QIODevice::WriteOnly) || target.write(data) != data.size() || !target.commit())
        return fail(error, "Cannot stage Brewfile");
    if (guestPath) *guestPath = "/.darling-launcher/Brewfile";
    return true;
}
QString issueDraft(const AppEntry &app, const QJsonArray &chain, const QString &output,
                   const QString &volume, const QString &prefix, const QString &darling) {
    QString text = "# Proposed VibeDarling issue: " + app.name + " launch dependency\n\n";
    text += "## Local provenance (review before sharing)\n- Source volume: `" + volume + "`\n- Source bundle: `" + volume + "/" + app.sourceRelative + "`\n- Prefix: `" + prefix + "`\n- Host launcher: `" + darling + "`\n\n";
    text += "## Reproduction\n1. Select the volume and prefix above.\n2. Import the bundle above.\n3. Run `DPREFIX='" + prefix + "' '" + darling + "' exec '/" + app.relativeBundle + "/Contents/MacOS/" + app.executable + "'`.\n\n";
    text += "## Dependency chain (loader interface output)\n";
    for (const auto &entry : chain) {
        auto item = entry.toObject();
        text += "- " + item.value("symbol").toString() + " expected in " + item.value("library").toString() + "; action: " + item.value("action").toString() + "\n";
    }
    text += "\n## Latest loader output\n```text\n" + output.left(8000) + "\n```\n\n";
    text += "Provenance: user-selected mounted macOS volume; app and library payloads remain private in the local prefix. No binary implementation was inspected.\n";
    return text;
}
}
