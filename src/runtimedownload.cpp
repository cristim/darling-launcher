// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtimedownload.h"
#include "releases.h"
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
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

namespace {
constexpr qint64 metadataLimit = 8 * 1024 * 1024;
constexpr qint64 archiveLimit = 16LL * 1024 * 1024 * 1024;
const QString repo = "VibeDarling/darling";
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
bool allowed(const QUrl &url, const QUrl &api) {
    if (!url.isValid() || !url.userInfo().isEmpty() || url.hasFragment()) return false;
    if (api.scheme() == "http" && api.host() == "127.0.0.1") return url.scheme() == "http" && url.host() == api.host() && url.port() == api.port();
    return api == QUrl("https://api.github.com") && url.scheme() == "https" && (url.port(-1) == -1 || url.port() == 443)
        && QStringList{"api.github.com", "github.com", "release-assets.githubusercontent.com", "objects.githubusercontent.com"}.contains(url.host());
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
        QTemporaryDir attempt(root + "/downloads/attempt-XXXXXX");
        if (!attempt.isValid()) return fail("Cannot create a private download attempt.");
        attempt.setAutoRemove(false);
        QNetworkAccessManager network;
        auto fetch = [&](QUrl url, qint64 limit, const QString &phase, const QString &file, QByteArray *bytes) -> QString {
            for (int hop = 0; hop < 6; ++hop) {
                if (stop()) return "Cancelled";
                if (!allowed(url, api)) return "Rejected download URL: " + url.toDisplayString(QUrl::RemoveUserInfo);
                QFile destination(file);
                if (!file.isEmpty() && !destination.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return "Cannot create " + file;
                if (bytes) bytes->clear();
                QNetworkRequest request(url);
                request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
                request.setRawHeader("User-Agent", "Darling-Launcher");
                auto *reply = network.get(request);
                reply->setReadBufferSize(1024 * 1024);
                QEventLoop loop; QTimer poll, deadline; poll.setInterval(100); deadline.setSingleShot(true);
                QString error; qint64 count = 0;
                auto drain = [&] {
                    if (!reply->isOpen()) return;
                    const auto chunk = reply->readAll(); count += chunk.size();
                    if (count > limit) { error = "Download exceeds the size limit for " + phase; reply->abort(); return; }
                    if (bytes) bytes->append(chunk);
                    if (destination.isOpen() && destination.write(chunk) != chunk.size()) { error = "Cannot write download: " + destination.errorString(); reply->abort(); }
                    report(phase, count, reply->header(QNetworkRequest::ContentLengthHeader).toLongLong());
                };
                connect(reply, &QIODevice::readyRead, &loop, drain);
                connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
                connect(&poll, &QTimer::timeout, &loop, [&] { if (stop()) reply->abort(); });
                connect(&deadline, &QTimer::timeout, &loop, [&] { error = "Download timed out: " + phase; reply->abort(); });
                poll.start(); deadline.start(file.isEmpty() ? 30000 : 30 * 60 * 1000);
                loop.exec(); drain(); destination.close();
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QUrl redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
                const auto networkError = reply->error(); const auto message = reply->errorString(); delete reply;
                if (stop()) return "Cancelled";
                if (!error.isEmpty()) return error;
                if (status >= 300 && status < 400 && !redirect.isEmpty()) {
                    // Each hop uses a new attempt file so redirects cannot overwrite prior bytes.
                    if (!file.isEmpty() && !QFile::remove(file)) return "Cannot discard the redirect response.";
                    url = url.resolved(redirect); continue;
                }
                if (networkError != QNetworkReply::NoError || status != 200) return "Download failed (HTTP " + QString::number(status) + "): " + message;
                return {};
            }
            return "Too many download redirects.";
        };
        QJsonObject release; QDateTime newest;
        for (int page = 1; page <= 5; ++page) {
            QByteArray bytes;
            const auto error = fetch(api.resolved(QUrl("/repos/" + repo + "/releases?per_page=100&page=" + QString::number(page))), metadataLimit, "Checking runtime releases", {}, &bytes);
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
        QProcess libraries; libraries.start("ldconfig", {"-p"});
        if (!libraries.waitForFinished(5000) || libraries.exitCode() != 0) return fail("Cannot inspect host libraries using ldconfig -p.");
        const auto installed = QString::fromLocal8Bit(libraries.readAllStandardOutput());
        for (const auto &name : requires.value("wrapped_sonames").toArray()) if (!name.isString() || !installed.contains('\n' + QString("\t") + name.toString() + ' ')) return fail("Missing host library: " + name.toString() + ". Build from source instead.");
        QStorageInfo storage(root); storage.refresh();
        if (!storage.isValid() || !storage.isReady() || artifact.unpackedSize > (storage.bytesAvailable() - artifact.size) / 1.2) return fail("Not enough free space for the archive and extracted runtime.");
        const QString archive = attempt.path() + '/' + artifact.file;
        error = fetch(QUrl(artifact.url), artifact.size, "Downloading runtime", archive, nullptr); if (!error.isEmpty()) return fail(error);
        if (QFileInfo(archive).size() != artifact.size) return fail("Downloaded runtime size differs from manifest.json.");
        report("Verifying and installing runtime");
        error = LauncherReleases::installArchive(archive, artifact.sha256, root + "/runtimes", selected.tag, gh, repo, stop);
        if (!error.isEmpty()) return fail(error);
        return {root + "/runtimes/" + selected.tag + "/usr/local", {}};
    }));
}
