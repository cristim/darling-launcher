// SPDX-License-Identifier: GPL-3.0-or-later
#include "releases.h"
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>
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
    if (artifact.size <= 0) return fail("manifest.json has no size for " + artifact.file + ".");
    const QUrl url(artifact.url, QUrl::StrictMode);
    static const QStringList downloadHosts{"github.com", "objects.githubusercontent.com"};
    if (!url.isValid() || url.scheme() != "https" || !downloadHosts.contains(url.host()) || url.port() != -1 || !url.userInfo().isEmpty())
        return fail("manifest.json url for " + artifact.file + " is not an https download from GitHub.");
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
    process.start(gh, {"attestation", "verify", archive, "--repo", repo, "--signer-workflow", signerWorkflow, "--source-ref", signerSourceRef, "--deny-self-hosted-runners"});
    if (!process.waitForFinished(120000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return "attestation verification failed for " + archive + " (needs a gh with --source-ref and --deny-self-hosted-runners): " + QString::fromUtf8(process.readAllStandardError());
    return {};
}
}

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QDirIterator>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <limits>
#include <climits>
#include <unistd.h>
namespace LauncherReleases {
// Copies archive to copy (created exclusively) and returns the SHA-256 of the bytes copied, so every later step
// reads exactly the bytes that were hashed. The final component of archive must not be a symlink.
static QString copyAndHash(const QString &archive, const QString &copy, QString *error) {
    const int fd = ::open(QFile::encodeName(archive).constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);  // O_NONBLOCK: opening a FIFO must not wait for a writer
    if (fd < 0) { *error = "cannot open " + archive + (errno == ELOOP ? QString(": it is a symlink") : ": " + QString::fromLocal8Bit(strerror(errno))); return {}; }
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) { ::close(fd); *error = archive + " is not a regular file"; return {}; }
    QFile source;
    if (!source.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) { ::close(fd); *error = "cannot read " + archive; return {}; }
    // Copy only the length it had when opened, so a file that keeps growing cannot fill the disk.
    qint64 remaining = info.st_size;
    QFile out(copy);
    if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { *error = "cannot create " + copy + ": " + out.errorString(); return {}; }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(1 << 20, Qt::Uninitialized);
    qint64 read = 0;
    while (remaining > 0 && (read = source.read(buffer.data(), qMin<qint64>(buffer.size(), remaining))) > 0) {
        remaining -= read;
        hash.addData(QByteArrayView(buffer.constData(), read));
        if (out.write(buffer.constData(), read) != read) { *error = "cannot write " + copy + ": " + out.errorString(); return {}; }
    }
    if (read < 0 || remaining > 0) { *error = "cannot read " + archive + ": " + source.errorString(); return {}; }
    if (!out.flush()) { *error = "cannot write " + copy + ": " + out.errorString(); return {}; }
    return hash.result().toHex();
}
static bool insideInstallRoot(const QString &path) { return path == "usr" || path == installRoot || path.startsWith(QString(installRoot) + '/'); }
// The only absolute symlinks a runtime may contain: exactly the 15 in the R1 pilot image
// (ci-binaries/local-pkg/darling-runtime-v0.test-linux-aarch64.tar.zst, `tar --zstd -tvf`). They resolve inside
// the guest root at run time. A new one in a future image must be reviewed and added here.
static const struct { const char *link, *target; } absoluteSymlinks[] = {
    {"usr/local/libexec/darling/usr/bin/erb", "/System/Library/Frameworks/Ruby.framework/Versions/2.6/usr/bin/erb"},
    {"usr/local/libexec/darling/usr/bin/gem", "/System/Library/Frameworks/Ruby.framework/Versions/2.6/usr/bin/gem"},
    {"usr/local/libexec/darling/usr/bin/irb", "/System/Library/Frameworks/Ruby.framework/Versions/2.6/usr/bin/irb"},
    {"usr/local/libexec/darling/usr/bin/rake", "/System/Library/Frameworks/Ruby.framework/Versions/2.6/usr/bin/rake"},
    {"usr/local/libexec/darling/usr/bin/rdoc", "/System/Library/Frameworks/Ruby.framework/Versions/2.6/usr/bin/rdoc"},
    {"usr/local/libexec/darling/usr/bin/ri", "/System/Library/Frameworks/Ruby.framework/Versions/2.6/usr/bin/ri"},
    {"usr/local/libexec/darling/usr/bin/ruby", "/System/Library/Frameworks/Ruby.framework/Versions/2.6/usr/bin/ruby"},
    {"usr/local/libexec/darling/usr/lib/libcom_err.dylib", "/System/Library/Frameworks/Kerberos.framework/Kerberos"},
    {"usr/local/libexec/darling/usr/lib/libdes425.dylib", "/System/Library/Frameworks/Kerberos.framework/Kerberos"},
    {"usr/local/libexec/darling/usr/lib/libgssapi_krb5.dylib", "/System/Library/Frameworks/Kerberos.framework/Kerberos"},
    {"usr/local/libexec/darling/usr/lib/libk5crypto.dylib", "/System/Library/Frameworks/Kerberos.framework/Kerberos"},
    {"usr/local/libexec/darling/usr/lib/libkrb4.dylib", "/System/Library/Frameworks/Kerberos.framework/Kerberos"},
    {"usr/local/libexec/darling/usr/lib/libkrb5.dylib", "/System/Library/Frameworks/Kerberos.framework/Kerberos"},
    {"usr/local/libexec/darling/usr/lib/libkrb524.dylib", "/System/Library/Frameworks/Kerberos.framework/Kerberos"},
    {"usr/local/libexec/darling/usr/lib/libkrb5support.dylib", "/System/Library/Frameworks/Kerberos.framework/Kerberos"},
};
QString symlinkTargetError(const QString &link, const QString &target) {
    if (target.startsWith('/')) {
        for (const auto &allowed : absoluteSymlinks)
            if (link == allowed.link && target == allowed.target) return {};
        return "symlink " + link + " has an absolute target that is not on the allow-list: " + target;
    }
    // Only leading ".." may climb, and only from the link's own folder, whose parents are real folders because
    // nothing is extracted below a symlink. A ".." after a name could climb out of a symlinked folder instead.
    const QStringList parts = target.split('/');
    qsizetype climb = 0;
    while (climb < parts.size() && parts[climb] == "..") ++climb;
    for (qsizetype i = climb; i < parts.size(); ++i)
        if (parts[i].isEmpty() || parts[i] == "." || parts[i] == ".." || parts[i].contains('\\')) return "symlink " + link + " has an unsupported target: " + target;
    const QString resolved = QDir::cleanPath(link.section('/', 0, -2) + '/' + target);
    if (resolved != installRoot && !resolved.startsWith(QString(installRoot) + '/')) return "symlink " + link + " points outside usr/local: " + target;
    return {};
}
QString vetMembers(const QStringList &listing, qint64 *regularBytes) {
    // Owners must be numeric: tar prints uname/gname unescaped, so a name with spaces could shift the path column.
    static const QRegularExpression row(R"(^(\S)\S{9}\s+\d+/\d+\s+(\d+)\s+\d{4}-\d\d-\d\d\s\d\d:\d\d\s(.+)$)");
    QSet<QString> symlinks;
    QString error;
    qint64 total = 0;
    for (const QString &line : listing) {
        if (line.isEmpty()) continue;
        const auto match = row.match(line);
        if (!match.hasMatch()) return "unparsable archive member: " + line;
        const QChar type = match.captured(1).at(0);
        QString path = match.captured(3);
        QString target;
        if (type == QLatin1Char('l')) {
            if (path.count(" -> ") != 1) return "archive symlink name or target contains ' -> ': " + path;
            target = path.section(" -> ", 1); path = path.section(" -> ", 0, 0);
        }
        else if (path.contains(" -> ")) return "archive member name contains ' -> ': " + path;
        if (path.contains('\\')) return "archive member name needs escaping: " + path;
        if (type == QLatin1Char('h') || type == QLatin1Char('c') || type == QLatin1Char('b') || type == QLatin1Char('p'))
            return "archive contains a hard link or device member: " + path;
        if (type != QLatin1Char('-') && type != QLatin1Char('d') && type != QLatin1Char('l')) return "unsupported archive member type: " + line;
        if (type == QLatin1Char('-')) {
            bool ok = false; const qint64 size = match.captured(2).toLongLong(&ok);
            if (!ok || size > std::numeric_limits<qint64>::max() - total) return "archive member size is out of range: " + line;
            total += size;
        }
        if (path.startsWith('/') || path.split('/').contains("..")) return "archive member escapes the runtime folder: " + path;
        path = QDir::cleanPath(path);
        if (!insideInstallRoot(path)) return "archive member is outside usr/local: " + path;
        // The path itself and each of its ancestors, so the check stays linear in the number of members.
        for (qsizetype end = path.size(); end > 0; end = path.lastIndexOf('/', end - 1)) {
            const QString prefix = path.left(end);
            if (symlinks.contains(prefix)) return "archive writes through the symlink " + prefix + ": " + path;
        }
        if (type == QLatin1Char('l')) {
            for (const QString protectedPath : {QString(launcherPath), QString(runtimeMarker)})
                if (protectedPath == path || protectedPath.startsWith(path + '/')) return "archive symlink replaces part of the runtime layout: " + path;
            error = symlinkTargetError(path, target);
            if (!error.isEmpty()) return "archive " + error;
            symlinks.insert(path);
        }
    }
    if (regularBytes) *regularBytes = total;
    return {};
}
QString runtimeLayoutError(const QString &dir) {
    const QString root = QFileInfo(dir).canonicalFilePath();
    if (root.isEmpty()) return dir + " does not exist";
    for (const char *relative : {installRoot, runtimeMarker, launcherPath})
        if (QFileInfo(dir + "/" + relative).canonicalFilePath() != root + "/" + relative) return QString(relative) + " is missing or leaves the runtime folder through a symlink";
    struct stat launcher;
    if (lstat(QFile::encodeName(dir + "/" + launcherPath).constData(), &launcher) != 0 || !S_ISREG(launcher.st_mode)) return QString(launcherPath) + " is not a regular file";
    // A hard link could be to the host's setuid darling; installArchive never produces either.
    if (launcher.st_nlink != 1) return QString(launcherPath) + " has another hard link";
    if (launcher.st_mode & (S_ISUID | S_ISGID)) return QString(launcherPath) + " is setuid or setgid";
    return {};
}
QString extractedTreeError(const QString &dir) {
    const QDir base(dir);
    QDirIterator entries(dir, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (entries.hasNext()) {
        const QString path = entries.next(), relative = base.relativeFilePath(path);
        if (!insideInstallRoot(relative)) return "extracted entry is outside usr/local: " + relative;
        struct stat info;
        if (lstat(QFile::encodeName(path).constData(), &info) != 0) return "cannot inspect extracted entry " + relative;
        // QDirIterator silently skips a folder it cannot open, so such a folder would hide its contents from this walk.
        if (S_ISDIR(info.st_mode) && (info.st_mode & S_IRWXU) != S_IRWXU) return "extracted folder is not readable, writable and searchable by its owner: " + relative;
        if (S_ISLNK(info.st_mode)) {
            QByteArray target(PATH_MAX, '\0');
            const ssize_t length = readlink(QFile::encodeName(path).constData(), target.data(), target.size());
            if (length < 0 || length >= target.size()) return "cannot read extracted symlink " + relative;
            target.truncate(length);
            const QString error = symlinkTargetError(relative, QFile::decodeName(target));
            if (!error.isEmpty()) return "extracted " + error;
            continue;
        }
        if (S_ISDIR(info.st_mode)) continue;
        if (!S_ISREG(info.st_mode)) return "extracted entry is not a file, folder or symlink: " + relative;
        if (info.st_nlink != 1) return "extracted file has another hard link: " + relative;
        if (info.st_mode & (S_ISUID | S_ISGID)) return "extracted file is setuid or setgid: " + relative;
    }
    return runtimeLayoutError(dir);
}
// Bytes in regular files below dir; symlinks are not followed.
static qint64 regularFileBytes(const QString &dir) {
    qint64 total = 0;
    QDirIterator entries(dir, QDir::Files | QDir::Hidden | QDir::System | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (entries.hasNext()) { entries.next(); total += entries.fileInfo().size(); }
    return total;
}
// Gives the owner rwx on every folder below dir, so an archive folder extracted read-only cannot block removal.
static void openFolders(const QString &dir) {
    struct stat info;
    if (lstat(QFile::encodeName(dir).constData(), &info) != 0 || !S_ISDIR(info.st_mode)) return;
    chmod(QFile::encodeName(dir).constData(), (info.st_mode & 07777) | S_IRWXU);
    for (const QString &name : QDir(dir).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) openFolders(dir + "/" + name);
}
QString installArchive(const QString &archive, const QString &sha256, const QString &runtimesRoot, const QString &tag, const QString &gh, const QString &repo, qint64 unpackedSize) {
    if (!validTag(tag)) return "not a release tag: " + tag;
    if (unpackedSize <= 0) return "release has no unpacked size";
    if (sha256.isEmpty()) return "release asset has no SHA-256 digest";
    if (!QDir().mkpath(runtimesRoot)) return "cannot create " + runtimesRoot;
    // A 0700 folder only this user can write: the copy in it cannot be swapped between hashing and extraction.
    const QTemporaryDir privateDir(runtimesRoot + "/.archive-XXXXXX");
    if (!privateDir.isValid()) return "cannot create a private folder in " + runtimesRoot + ": " + privateDir.errorString();
    const QString copy = privateDir.filePath("runtime.tar.zst");
    QString error;
    const QString actual = copyAndHash(archive, copy, &error);
    if (!error.isEmpty()) return error;
    if (actual.compare(sha256, Qt::CaseInsensitive) != 0) return "SHA-256 mismatch for " + archive + ": expected " + sha256 + ", got " + actual;
    error = verifyAttestation(gh, copy, repo);
    if (!error.isEmpty()) return error;
    const QString target = runtimesRoot + "/" + tag;
    if (QFileInfo::exists(target)) return target + " already exists";
    // A fixed locale, so whether a non-ASCII name is escaped (and refused) does not depend on the user's LANG.
    QProcessEnvironment tarEnvironment = QProcessEnvironment::systemEnvironment();
    tarEnvironment.insert("LC_ALL", "C");
    QProcess list;
    list.setProcessEnvironment(tarEnvironment);
    list.start("tar", {"--zstd", "--quoting-style=escape", "--numeric-owner", "-tvf", copy});
    if (!list.waitForFinished(600000) || list.exitStatus() != QProcess::NormalExit || list.exitCode() != 0)
        return "cannot list " + archive + ": " + QString::fromUtf8(list.readAllStandardError());
    qint64 listedBytes = 0;
    error = vetMembers(QString::fromUtf8(list.readAllStandardOutput()).split('\n'), &listedBytes);
    if (!error.isEmpty()) return error;
    if (listedBytes > unpackedSize) return "archive unpacks to " + QString::number(listedBytes) + " bytes, more than the " + QString::number(unpackedSize) + " in manifest.json";
    const qint64 extractionLimit = unpackedSize + extractionSlack;
    const QString staging = target + ".partial";
    // mkdir fails when the folder exists, so creating staging is the exclusive claim on this tag.
    if (!QDir().mkdir(staging)) return "cannot create " + staging + ": another install of " + tag + " is running, or an earlier one left it behind";
    // Staging is ours from here on; removing it on failure lets a retry claim it again. Symlinks are removed, not followed.
    auto abandon = [&](const QString &reason) { openFolders(staging); return QDir(staging).removeRecursively() ? reason : reason + "; cannot remove " + staging + ", remove it by hand"; };
    if (QFileInfo::exists(target)) return abandon(target + " already exists");
    QProcess extract;
    extract.setProcessEnvironment(tarEnvironment);
    extract.start("tar", {"--zstd", "-xf", copy, "-C", staging, "--no-same-owner", "--no-same-permissions", "--no-overwrite-dir"});
    // Defence in depth against a tar or zstd bug: the listing check above already bounds what GNU tar writes.
    // Each scan stats the whole tree (tens of ms for the full image), hence the 2 s interval.
    QElapsedTimer clock; clock.start();
    while (!extract.waitForFinished(2000)) {
        QString stop;
        if (clock.elapsed() > 1800000) stop = "extraction timed out";
        else if (regularFileBytes(staging) > extractionLimit) stop = "extraction wrote more than " + QString::number(extractionLimit) + " bytes";
        if (!stop.isEmpty()) { extract.kill(); extract.waitForFinished(); return abandon(stop); }
    }
    if (regularFileBytes(staging) > extractionLimit) return abandon("extraction wrote more than " + QString::number(extractionLimit) + " bytes");
    if (extract.exitStatus() != QProcess::NormalExit || extract.exitCode() != 0)
        return abandon("extraction failed: " + QString::fromUtf8(extract.readAllStandardError()));
    error = extractedTreeError(staging);
    if (!error.isEmpty()) return abandon(error);
    if (!QDir().rename(staging, target)) return abandon("cannot move " + staging + " to " + target);
    return {};
}
}
