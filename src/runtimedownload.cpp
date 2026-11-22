// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtimedownload.h"
#include "releases.h"
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QTimer>
#include <QVersionNumber>
#include <QRegularExpression>
#include <QtConcurrent>
#include <gnu/libc-version.h>
#include <limits>
#include <QCryptographicHash>
#include <QUuid>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>

namespace {
constexpr qint64 metadataLimit = 8 * 1024 * 1024;
constexpr qint64 archiveLimit = 16LL * 1024 * 1024 * 1024;
const QString repo = LauncherReleases::repository;
QJsonObject asset(const QJsonObject &release, const QString &name) {
    QJsonObject found;
    for (const auto &entry : release.value("assets").toArray()) {
        const auto object = entry.toObject();
        if (object.value("name").toString() == name) {
            if (!found.isEmpty()) return {};
            found = object;
        }
    }
    return found;
}
bool allowed(const QUrl &url) {
    if (!url.isValid() || !url.userInfo().isEmpty() || url.hasFragment()) return false;
    return url.scheme() == "https" && (url.port(-1) == -1 || url.port() == 443)
        && QStringList{"api.github.com", "github.com", "release-assets.githubusercontent.com", "objects.githubusercontent.com"}.contains(url.host());
}
QString privateDirectory(const QString &path) {
    if (::mkdir(QFile::encodeName(path).constData(), 0700) != 0 && errno != EEXIST) return "Cannot create a private archive cache directory.";
    struct stat info;
    if (lstat(QFile::encodeName(path).constData(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != getuid() || (info.st_mode & 07777) != 0700) return "Archive cache directory must be a real user-owned directory with mode 0700.";
    return {};
}
QString openPartial(QFile &file, const QString &path, bool exclusive = false) {
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC | (exclusive ? O_EXCL : 0), 0600);
    if (fd < 0) return "Cannot open the retained archive partial without following links.";
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != getuid() || info.st_nlink != 1 || (info.st_mode & 07777) != 0600) { ::close(fd); return "Archive partial must be a private user-owned regular file."; }
    if (!file.open(fd, QIODevice::ReadWrite, QFileDevice::AutoCloseHandle)) { ::close(fd); return "Cannot open the retained archive partial."; }
    if (!file.seek(info.st_size)) return "Cannot seek to the retained archive partial.";
    return {};
}
QString retainPartial(QFile &file, const QString &path) {
    file.close();
    const QString retained = QFileInfo(path).absolutePath() + "/retained-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".part";
    return QFile::rename(path, retained) ? QString() : "Cannot preserve the rejected archive partial.";
}
QString diskSpaceError(const QString &root, qint64 size, qint64 unpacked, qint64 offset) {
    const auto maximum = std::numeric_limits<qint64>::max();
    if (unpacked > maximum - LauncherReleases::extractionSlack || size > maximum - unpacked - LauncherReleases::extractionSlack || size - offset > maximum - unpacked - LauncherReleases::extractionSlack - size) return "Runtime disk requirement is out of range.";
    const qint64 required = size - offset + size + unpacked + LauncherReleases::extractionSlack;
    QStorageInfo storage(root); storage.refresh();
    return !storage.isValid() || !storage.isReady() || required > storage.bytesAvailable() ? "Not enough free space for the remaining archive, private verification copy and extracted runtime." : QString();
}
}

RuntimeDownload::RuntimeDownload(QObject *parent, QUrl endpoint) : QObject(parent), api(std::move(endpoint)) {
    connect(&watcher, &QFutureWatcher<Result>::finished, this, [this] {
        const auto result = watcher.result(); emit completed(result.runtime, result.error);
    });
}
RuntimeDownload::~RuntimeDownload() { cancel(); watcher.waitForFinished(); }
void RuntimeDownload::cancel() { cancelled = true; }
bool RuntimeDownload::isRunning() const { return watcher.isRunning(); }
void RuntimeDownload::start(const QString &root) {
    if (isRunning()) return;
    cancelled = false;
    watcher.setFuture(QtConcurrent::run([this, root]() -> Result {
        auto stop = [this] { return cancelled.load(); };
        auto report = [this](const QString &phase, qint64 received = 0, qint64 total = 0) {
            emit progress(phase, received, total);
        };
        auto fail = [&](const QString &error) -> Result { return {{}, stop() ? "Cancelled" : error}; };
        const QString gh = QStandardPaths::findExecutable("gh");
        if (gh.isEmpty()) return fail("GitHub CLI (gh) is required to verify runtime attestations. Install it or build from source.");
        if (!QDir().mkpath(root + "/downloads")) return fail("Cannot create the private download directory.");
        struct stat downloads;
        if (lstat(QFile::encodeName(root + "/downloads").constData(), &downloads) != 0 || !S_ISDIR(downloads.st_mode) || downloads.st_uid != getuid() || (downloads.st_mode & (S_IWGRP | S_IWOTH))) return fail("Downloads must be a real user-owned directory without shared write access.");
        QTemporaryDir attempt(root + "/downloads/attempt-XXXXXX");
        if (!attempt.isValid()) return fail("Cannot create a private download attempt.");
        attempt.setAutoRemove(false);
        QNetworkAccessManager network;
        auto fetch = [&](QUrl url, qint64 limit, const QString &phase, const QString &file, QByteArray *bytes, QFile *partial = nullptr, qint64 unpacked = 0) -> QString {
            for (int hop = 0; hop < 6; ++hop) {
                if (stop()) return "Cancelled";
                if (!allowed(url)) return "Rejected download URL: " + url.toDisplayString(QUrl::RemoveUserInfo);
                QFile destination(file);
                if (!partial && !file.isEmpty() && !destination.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return "Cannot create " + file;
                if (bytes) bytes->clear();
                QUrl transport = url;
                if (api.scheme() == "http" && api.host() == "127.0.0.1" && api.userInfo().isEmpty() && api.path().isEmpty() && !api.hasQuery() && !api.hasFragment()) {
                    transport.setScheme(api.scheme()); transport.setHost(api.host()); transport.setPort(api.port());
                } else if (api != QUrl("https://api.github.com")) return "Invalid runtime API endpoint.";
                QNetworkRequest request(transport);
                request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
                request.setRawHeader("User-Agent", "Darling-Launcher");
                request.setRawHeader("Accept-Encoding", "identity");
                qint64 offset = partial ? partial->size() : 0;
                if (offset > 0) request.setRawHeader("Range", "bytes=" + QByteArray::number(offset) + '-');
                auto *reply = network.get(request);
                reply->setReadBufferSize(1024 * 1024);
                QEventLoop loop; QTimer poll, deadline; poll.setInterval(100); deadline.setSingleShot(true);
                QString error; qint64 count = 0; bool redirected = false, accepted = false, invalidPartial = false; QUrl next;
                auto headers = [&] {
                    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    if (status == 0 || accepted || !error.isEmpty()) return;
                    if (status >= 300 && status < 400) {
                        next = url.resolved(reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl());
                        if (reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl().isEmpty() || !allowed(next)) error = "Rejected download redirect.";
                        else redirected = true;
                        reply->abort(); return;
                    }
                    const QByteArray encoding = reply->rawHeader("Content-Encoding");
                    if (!encoding.isEmpty() && encoding != "identity") error = "Rejected nonidentity download encoding.";
                    else if (partial && offset > 0 && status == 200) {
                        error = diskSpaceError(root, limit, unpacked, 0);
                        if (error.isEmpty()) error = retainPartial(*partial, file);
                        if (error.isEmpty()) error = openPartial(*partial, file, true);
                        if (error.isEmpty()) offset = 0;
                    } else if (partial && offset > 0) {
                        int ranges = 0; for (const auto &header : reply->rawHeaderPairs()) if (header.first.toLower() == "content-range") ++ranges;
                        const QByteArray expected = "bytes " + QByteArray::number(offset) + '-' + QByteArray::number(limit - 1) + '/' + QByteArray::number(limit);
                        if (status != 206 || ranges != 1 || reply->rawHeader("Content-Range") != expected) error = "Rejected archive partial response: Content-Range does not match the retained bytes.";
                    } else if (status != 200) error = "Invalid download response for " + phase;
                    if (error.isEmpty() && partial && reply->hasRawHeader("Content-Length") && reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() != limit - offset) { error = "Rejected archive partial response: Content-Length does not match the remaining signed size."; invalidPartial = true; }
                    if (!error.isEmpty()) { invalidPartial = invalidPartial || (partial && (status == 206 || !encoding.isEmpty())); reply->abort(); return; }
                    accepted = true;
                };
                auto drain = [&] {
                    if (!reply->isOpen()) return;
                    headers(); if (!accepted || !error.isEmpty()) return;
                    const auto chunk = reply->readAll(); count += chunk.size();
                    if (count > limit - offset) { error = "Download exceeds the size limit for " + phase; invalidPartial = partial; reply->abort(); return; }
                    if (bytes) bytes->append(chunk);
                    QFile *output = partial ? partial : &destination;
                    if (output->isOpen() && output->write(chunk) != chunk.size()) { error = "Cannot write download: " + output->errorString(); reply->abort(); }
                    report(phase, offset + count, partial ? limit : reply->header(QNetworkRequest::ContentLengthHeader).toLongLong());
                };
                connect(reply, &QNetworkReply::metaDataChanged, &loop, headers);
                connect(reply, &QIODevice::readyRead, &loop, drain);
                connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
                connect(&poll, &QTimer::timeout, &loop, [&] { if (stop()) reply->abort(); });
                connect(&deadline, &QTimer::timeout, &loop, [&] { error = "Download timed out: " + phase; reply->abort(); });
                poll.start(); deadline.start(file.isEmpty() && !partial ? 30000 : 30 * 60 * 1000);
                loop.exec(); drain(); destination.close();
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const auto networkError = reply->error(); const auto message = reply->errorString(); delete reply;
                if (invalidPartial) { const QString retained = retainPartial(*partial, file); if (!retained.isEmpty()) return retained; }
                if (stop()) return "Cancelled";
                if (!error.isEmpty()) return error;
                if (redirected) {
                    // Each hop uses a new attempt file so redirects cannot overwrite prior bytes.
                    if (!partial && !file.isEmpty() && !QFile::remove(file)) return "Cannot discard the redirect response.";
                    url = next; continue;
                }
                if (networkError != QNetworkReply::NoError || !accepted) return "Download failed (HTTP " + QString::number(status) + "): " + message;
                if (partial && offset + count != limit) return "Downloaded archive partial is truncated.";
                if (partial && !partial->flush()) return "Cannot flush the retained archive partial.";
                return {};
            }
            return "Too many download redirects.";
        };
        QJsonObject release; QDateTime newest;
        for (int page = 1; page <= 5; ++page) {
            QByteArray bytes;
            const auto error = fetch(QUrl("https://api.github.com/repos/" + repo + "/releases?per_page=100&page=" + QString::number(page)), metadataLimit, "Checking runtime releases", {}, &bytes);
            if (!error.isEmpty()) return fail(error);
            QJsonParseError parse; const auto document = QJsonDocument::fromJson(bytes, &parse);
            if (parse.error != QJsonParseError::NoError || !document.isArray()) return fail("GitHub returned an invalid release list.");
            const auto entries = document.array();
            for (const auto &entry : entries) {
                const auto candidate = entry.toObject();
                if (candidate.value("draft").toBool() || candidate.value("prerelease").toBool() || !LauncherReleases::validTag(candidate.value("tag_name").toString())) continue;
                bool complete = true;
                for (const auto &name : {"manifest.json", "SHA256SUMS", "refs.lock.json", "nested.refs.lock.json"}) complete &= !asset(candidate, name).isEmpty();
                const auto published = QDateTime::fromString(candidate.value("published_at").toString(), Qt::ISODate);
                if (complete && published.isValid() && (!newest.isValid() || published > newest)) { release = candidate; newest = published; }
            }
            if (entries.size() < 100) break;
            if (page == 5) return fail("Release listing exceeds the supported page limit. Build from source.");
        }
        if (release.isEmpty()) return fail("No stable prebuilt runtime release is available. Build from source instead.");
        auto metadata = [&](const QString &name, QByteArray *bytes) -> QString {
            const QString path = attempt.path() + '/' + name;
            QString error = fetch(QUrl(asset(release, name).value("browser_download_url").toString()), metadataLimit, "Downloading " + name, path, nullptr);
            if (!error.isEmpty()) return error;
            const auto declared = asset(release, name).value("size");
            if (!declared.isUndefined() && (declared.toInteger(-1) < 0 || QFileInfo(path).size() != declared.toInteger(-1))) return "Downloaded metadata size differs from the release asset: " + name;
            report("Verifying " + name);
            error = LauncherReleases::verifyAttestation(gh, path, repo, stop);
            if (!error.isEmpty()) return error;
            QFile file(path); if (!file.open(QIODevice::ReadOnly)) return file.errorString(); *bytes = file.readAll(); return {};
        };
        QByteArray manifestBytes, sums;
        QString error = metadata("manifest.json", &manifestBytes); if (!error.isEmpty()) return fail(error);
        error = metadata("SHA256SUMS", &sums); if (!error.isEmpty()) return fail(error);
        QJsonParseError parse; const auto manifest = QJsonDocument::fromJson(manifestBytes, &parse);
        if (parse.error != QJsonParseError::NoError || !manifest.isObject()) return fail("Invalid manifest.json.");
        const auto selected = LauncherReleases::select(release, manifest.object(), LauncherReleases::hostArchitecture(), false);
        if (!selected.valid()) return fail(selected.error + " Build from source instead.");
        const auto artifact = selected.artifact;
        if (artifact.size <= 0 || artifact.size > archiveLimit || asset(release, artifact.file).value("size").toInteger() != artifact.size) return fail("Runtime size is invalid or disagrees with the release asset.");
        int checksums = 0;
        for (const auto &line : sums.split('\n')) if (line.size() >= 66 && (line.mid(64, 2) == "  " || line.mid(64, 2) == " *") && line.mid(66) == artifact.file.toUtf8()) {
            ++checksums; if (line.left(64).toLower() != artifact.sha256.toLatin1()) return fail("SHA256SUMS disagrees with manifest.json.");
        }
        if (checksums != 1) return fail("SHA256SUMS does not uniquely identify the runtime archive.");
        const auto requires = manifest.object().value("artifacts").toObject().value(LauncherReleases::hostArchitecture()).toObject().value("host_requires").toObject();
        const auto glibc = requires.value("glibc").toString();
        if (!QRegularExpression("^[0-9]+\\.[0-9]+(\\.[0-9]+)?$").match(glibc).hasMatch() || QVersionNumber::fromString(glibc).toString() != glibc) return fail("manifest.json has an invalid glibc requirement.");
        if (QVersionNumber::compare(QVersionNumber::fromString(QString::fromLatin1(gnu_get_libc_version())), QVersionNumber::fromString(glibc)) < 0) return fail("This runtime requires glibc " + glibc + ". Build from source on this host.");
        if (!requires.value("wrapped_sonames").isArray()) return fail("manifest.json has no host library requirements.");
        report("Checking host libraries"); QProcess libraries; libraries.start("ldconfig", {"-p"});
        QElapsedTimer libraryClock; libraryClock.start(); QByteArray libraryBytes; qint64 libraryErrors = 0;
        do {
            libraries.waitForFinished(100);
            libraryBytes += libraries.readAllStandardOutput();
            libraryErrors += libraries.readAllStandardError().size();
            QString reason;
            if (stop()) reason = "Cancelled";
            else if (libraries.error() == QProcess::FailedToStart) reason = "failed to start";
            else if (libraryClock.elapsed() > 5000) reason = "timed out";
            else if (libraryBytes.size() > metadataLimit || libraryErrors > metadataLimit) reason = "output limit exceeded";
            if (!reason.isEmpty()) { libraries.kill(); libraries.waitForFinished(5000); return fail("Host library inspection " + reason + '.'); }
        } while (libraries.state() != QProcess::NotRunning);
        if (libraries.exitStatus() != QProcess::NormalExit || libraries.exitCode() != 0) return fail("Host library inspection exited unsuccessfully.");
        const auto installed = QString::fromLocal8Bit(libraryBytes);
        for (const auto &name : requires.value("wrapped_sonames").toArray()) if (!name.isString() || !installed.contains('\n' + QString("\t") + name.toString() + ' ')) return fail("Missing host library: " + name.toString() + ". Build from source instead.");
        const QJsonObject identity{{"tag", selected.tag}, {"arch", LauncherReleases::hostArchitecture()}, {"file", artifact.file}, {"url", artifact.url}, {"sha256", artifact.sha256}, {"size", artifact.size}};
        const QString identityHash = QCryptographicHash::hash(QJsonDocument(identity).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex();
        const QString tagDirectory = root + "/downloads/" + selected.tag, cache = tagDirectory + '/' + identityHash;
        error = privateDirectory(tagDirectory); if (error.isEmpty()) error = privateDirectory(cache); if (!error.isEmpty()) return fail(error);
        const int lockFd = ::open(QFile::encodeName(cache + "/archive.lock").constData(), O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
        if (lockFd < 0) return fail("Cannot open the private archive download lock.");
        const struct Lock { int fd; ~Lock() { ::close(fd); } } lock{lockFd}; struct stat lockInfo;
        if (fstat(lockFd, &lockInfo) != 0 || !S_ISREG(lockInfo.st_mode) || lockInfo.st_uid != getuid() || lockInfo.st_nlink != 1 || (lockInfo.st_mode & 07777) != 0600) return fail("Archive download lock must be a private regular file.");
        if (flock(lockFd, LOCK_EX | LOCK_NB) != 0) return fail("Another download of this signed runtime is already running. Retry after it finishes.");
        const QString archivePath = cache + "/archive.part";
        QFile archive; error = openPartial(archive, archivePath); if (!error.isEmpty()) return fail(error);
        if (archive.size() > artifact.size) { error = retainPartial(archive, archivePath); return fail(error.isEmpty() ? "Retained archive partial exceeds its signed size; retry to download a fresh copy." : error); }
        error = diskSpaceError(root, artifact.size, artifact.unpackedSize, archive.size()); if (!error.isEmpty()) return fail(error);
        if (archive.size() < artifact.size) { error = fetch(QUrl(artifact.url), artifact.size, "Downloading runtime", archivePath, nullptr, &archive, artifact.unpackedSize); if (!error.isEmpty()) return fail(error); }
        if (archive.size() != artifact.size) return fail("Downloaded runtime size differs from manifest.json.");
        report("Verifying and installing runtime");
        bool digestMismatch = false;
        error = LauncherReleases::installArchive(archivePath, artifact.sha256, root + "/runtimes", selected.tag, gh, repo, artifact.unpackedSize, stop, &digestMismatch);
        if (digestMismatch) { const QString retained = retainPartial(archive, archivePath); if (!retained.isEmpty()) return fail(retained); }
        if (!error.isEmpty()) return fail(error);
        return {root + "/runtimes/" + selected.tag + "/usr/local", {}};
    }));
}
