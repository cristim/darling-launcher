// SPDX-License-Identifier: GPL-3.0-or-later
#include "releases.h"
#include <QJsonObject>
#include <QRegularExpression>
#include <sys/utsname.h>
#include <QDir>
#include <QJsonArray>
#include <QProcess>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QFile>
namespace LauncherReleases {
bool validTag(const QString &tag) {
    static const QRegularExpression pattern(R"(^v\d{4}\.\d\d\.\d\d-[0-9a-f]{7}(-r\d+)?$)");
    return pattern.match(tag).hasMatch();
}
QString hostArchitecture() {
    struct utsname name;
    return uname(&name) == 0 ? QString::fromLatin1(name.machine) : QString();
}
static Selection fail(const QString &message) { Selection result; result.error = message; return result; }
Selection select(const QJsonObject &release, const QJsonObject &manifest, const QString &arch, bool allowPrerelease) {
    const QString tag = release.value("tag_name").toString();
    if (!validTag(tag)) return fail("Latest release tag '" + tag + "' is not vYYYY.MM.DD-<sha7>.");
    if (release.value("draft").toBool()) return fail("Release " + tag + " is a draft.");
    if ((release.value("prerelease").toBool() || manifest.value("prerelease").toBool()) && !allowPrerelease) return fail("Release " + tag + " is a prerelease; opt in to use it.");
    if (manifest.value("schema").toInt() != 1) return fail("manifest.json schema " + manifest.value("schema").toVariant().toString() + " is not supported.");
    if (manifest.value("version").toString() != tag) return fail("manifest.json version does not match release " + tag + ".");
    if (arch.isEmpty()) return fail("Cannot determine the host architecture.");
    const QJsonValue entry = manifest.value("artifacts").toObject().value(arch);
    if (!entry.isObject()) return fail("Release " + tag + " has no runtime for " + arch + ".");
    const QJsonObject object = entry.toObject();
    Artifact artifact{object.value("file").toString(), object.value("url").toString(), object.value("sha256").toString().toLower(), object.value("install_root").toString(), object.value("launcher").toString(),
                      object.value("size").toInteger(), object.value("unpacked_size").toInteger()};
    if (artifact.installRoot != installRoot || artifact.launcher != launcherPath) return fail("manifest.json install_root/launcher are not " + QString(installRoot) + " and " + launcherPath + ".");
    if (artifact.file != "darling-runtime-" + tag + "-linux-" + arch + ".tar.zst") return fail("manifest.json file name " + artifact.file + " does not match the release and architecture.");
    static const QRegularExpression hex("^[0-9a-f]{64}$");
    if (!hex.match(artifact.sha256).hasMatch()) return fail("manifest.json has no valid SHA-256 for " + artifact.file + ".");
    if (artifact.unpackedSize <= 0) return fail("manifest.json has no unpacked_size for " + artifact.file + ".");
    bool listed = false;
    for (const auto &value : release.value("assets").toArray()) {
        const auto asset = value.toObject();
        if (asset.value("name").toString() != artifact.file) continue;
        listed = true;
        if (asset.value("browser_download_url").toString() != artifact.url) return fail("manifest.json url differs from the release asset URL for " + artifact.file + ".");
        const QString digest = asset.value("digest").toString();
        if (!digest.isEmpty() && digest.toLower() != "sha256:" + artifact.sha256) return fail("SHA-256 in manifest.json differs from the GitHub digest of " + artifact.file + ".");
    }
    if (!listed) return fail("Release " + tag + " does not list " + artifact.file + ".");
    Selection result; result.tag = tag; result.artifact = artifact; return result;
}
QString verifyAttestation(const QString &gh, const QString &archive, const QString &repo) {
    if (gh.isEmpty()) return "gh is required to verify the build attestation; install GitHub CLI or build from source";
    QProcess process;
    process.start(gh, {"attestation", "verify", archive, "--repo", repo, "--signer-workflow", repo + "/.github/workflows/release-binaries.yml"});
    if (!process.waitForFinished(120000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return "attestation verification failed for " + archive + ": " + QString::fromUtf8(process.readAllStandardError());
    return {};
}
}

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
namespace LauncherReleases {
static QString sha256Of(const QString &path, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { *error = "cannot read " + path + ": " + file.errorString(); return {}; }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) { *error = "cannot hash " + path; return {}; }
    return hash.result().toHex();
}
QString vetMembers(const QStringList &listing) {
    // Owners must be numeric: tar prints uname/gname unescaped, so a name with spaces could shift the path column.
    static const QRegularExpression row(R"(^(\S)\S{9}\s+\d+/\d+\s+\d+\s+\d{4}-\d\d-\d\d\s\d\d:\d\d\s(.+)$)");
    QStringList symlinks;
    for (const QString &line : listing) {
        if (line.isEmpty()) continue;
        const auto match = row.match(line);
        if (!match.hasMatch()) return "unparsable archive member: " + line;
        const QChar type = match.captured(1).at(0);
        QString path = match.captured(2);
        if (type == QLatin1Char('l')) { if (path.count(" -> ") != 1) return "archive symlink name or target contains ' -> ': " + path; path = path.section(" -> ", 0, 0); }
        else if (path.contains(" -> ")) return "archive member name contains ' -> ': " + path;
        if (path.contains('\\')) return "archive member name needs escaping: " + path;
        if (type == QLatin1Char('h') || type == QLatin1Char('c') || type == QLatin1Char('b') || type == QLatin1Char('p'))
            return "archive contains a hard link or device member: " + path;
        if (type != QLatin1Char('-') && type != QLatin1Char('d') && type != QLatin1Char('l')) return "unsupported archive member type: " + line;
        if (path.startsWith('/') || path.split('/').contains("..")) return "archive member escapes the runtime folder: " + path;
        path = QDir::cleanPath(path);
        if (path != "usr" && path != installRoot && !path.startsWith(QString(installRoot) + '/')) return "archive member is outside usr/local: " + path;
        for (const QString &link : symlinks)
            if (path == link || path.startsWith(link + '/')) return "archive writes through the symlink " + link + ": " + path;
        if (type == QLatin1Char('l')) symlinks << path;
    }
    return {};
}
QString installArchive(const QString &archive, const QString &sha256, const QString &runtimesRoot, const QString &tag, const QString &gh, const QString &repo) {
    if (!validTag(tag)) return "not a release tag: " + tag;
    if (sha256.isEmpty()) return "release asset has no SHA-256 digest";
    QString error;
    const QString actual = sha256Of(archive, &error);
    if (!error.isEmpty()) return error;
    if (actual.compare(sha256, Qt::CaseInsensitive) != 0) return "SHA-256 mismatch for " + archive + ": expected " + sha256 + ", got " + actual;
    error = verifyAttestation(gh, archive, repo);
    if (!error.isEmpty()) return error;
    const QString target = runtimesRoot + "/" + tag;
    if (QFileInfo::exists(target)) return target + " already exists";
    QProcess list;
    list.start("tar", {"--zstd", "--quoting-style=escape", "--numeric-owner", "-tvf", archive});
    if (!list.waitForFinished(600000) || list.exitStatus() != QProcess::NormalExit || list.exitCode() != 0)
        return "cannot list " + archive + ": " + QString::fromUtf8(list.readAllStandardError());
    error = vetMembers(QString::fromUtf8(list.readAllStandardOutput()).split('\n'));
    if (!error.isEmpty()) return error;
    const QString staging = target + ".partial";
    if (QFileInfo::exists(staging)) return staging + " is left over from an earlier attempt; remove it by hand";
    if (!QDir().mkpath(staging)) return "cannot create " + staging;
    QProcess extract;
    extract.start("tar", {"--zstd", "-xf", archive, "-C", staging, "--no-same-owner", "--no-same-permissions", "--no-overwrite-dir"});
    if (!extract.waitForFinished(1800000) || extract.exitStatus() != QProcess::NormalExit || extract.exitCode() != 0)
        return "extraction failed, partial files left in " + staging + ": " + QString::fromUtf8(extract.readAllStandardError());
    const QFileInfo launcher(staging + "/" + launcherPath);
    if (launcher.isSymLink() || !launcher.isFile())
        return "archive has no regular (non-symlink) usr/local/bin/darling, partial files left in " + staging;
    if (!QDir().rename(staging, target)) return "cannot move " + staging + " to " + target;
    return {};
}
}
