// SPDX-License-Identifier: GPL-3.0-or-later
#include "releases.h"
#include <QJsonObject>
#include <QRegularExpression>
#include <QSysInfo>
#include <algorithm>
namespace LauncherReleases {
std::optional<Tag> parseTag(const QString &tag) {
    static const QRegularExpression pattern("^(\\d{4})-(\\d{2})-(\\d{2})-(\\d{2})-(\\d{2})-([0-9a-f]{7,40})$");
    const auto match = pattern.match(tag);
    if (!match.hasMatch()) return std::nullopt;
    const QDate date(match.captured(1).toInt(), match.captured(2).toInt(), match.captured(3).toInt());
    const QTime clock(match.captured(4).toInt(), match.captured(5).toInt());
    if (!date.isValid() || !clock.isValid()) return std::nullopt;
    const QDateTime time(date, clock, Qt::UTC);
    if (!time.isValid()) return std::nullopt;
    return Tag{time, match.captured(6)};
}
QString assetName(const QString &component, const QString &arch) { return "darling-" + component + "-" + arch + ".tar.zst"; }
QString hostArchitecture() { return QSysInfo::currentCpuArchitecture(); }
Selection latest(const QJsonArray &releases, const QString &component, const QString &arch) {
    Selection result;
    QList<Release> found;
    for (const auto &value : releases) {
        const auto object = value.toObject();
        if (object.value("draft").toBool()) continue;
        const auto tag = parseTag(object.value("tag_name").toString());
        if (!tag) continue;
        Release release{object.value("tag_name").toString(), tag->time, QDateTime::fromString(object.value("published_at").toString(), Qt::ISODate), tag->sha, {}};
        for (const auto &assetValue : object.value("assets").toArray()) {
            const auto asset = assetValue.toObject();
            QString digest = asset.value("digest").toString();
            release.assets.append({asset.value("name").toString(), asset.value("browser_download_url").toString(), digest.startsWith("sha256:") ? digest.mid(7) : QString()});
        }
        found.append(release);
    }
    if (found.isEmpty()) { result.error = "No usable release was found (tags must look like YYYY-MM-DD-HH-MM-<sha>)."; return result; }
    std::sort(found.begin(), found.end(), [](const Release &a, const Release &b) { return a.time != b.time ? a.time > b.time : a.published > b.published; });
    if (found.size() > 1 && found[0].time == found[1].time && found[0].published == found[1].published) {
        result.error = "Releases " + found[0].tag + " and " + found[1].tag + " cannot be ordered."; return result;
    }
    result.release = found.first();
    const QString wanted = assetName(component, arch);
    for (const auto &asset : result.release.assets) if (asset.name == wanted) result.asset = asset;
    if (result.asset.name.isEmpty()) result.error = "Release " + result.release.tag + " has no " + wanted + " asset.";
    else if (result.asset.url.isEmpty() || result.asset.sha256.isEmpty()) result.error = "Release " + result.release.tag + " lists " + wanted + " without a download URL or SHA-256 digest.";
    return result;
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
    static const QRegularExpression row(R"(^(\S)\S+\s+\S+\s+\d+\s+\d{4}-\d\d-\d\d\s+\d\d:\d\d\s(.+)$)");
    QStringList symlinks;
    for (const QString &line : listing) {
        if (line.isEmpty()) continue;
        const auto match = row.match(line);
        if (!match.hasMatch()) return "unparsable archive member: " + line;
        const QChar type = match.captured(1).at(0);
        QString path = match.captured(2);
        if (type == QLatin1Char('l')) path = path.section(" -> ", 0, 0);
        if (path.contains('\\')) return "archive member name needs escaping: " + path;
        if (type == QLatin1Char('h') || type == QLatin1Char('c') || type == QLatin1Char('b') || type == QLatin1Char('p'))
            return "archive contains a hard link or device member: " + path;
        if (type != QLatin1Char('-') && type != QLatin1Char('d') && type != QLatin1Char('l')) return "unsupported archive member type: " + line;
        if (path.startsWith('/') || path.split('/').contains("..")) return "archive member escapes the runtime folder: " + path;
        path = QDir::cleanPath(path);
        for (const QString &link : symlinks)
            if (path == link || path.startsWith(link + '/')) return "archive writes through the symlink " + link + ": " + path;
        if (type == QLatin1Char('l')) symlinks << path;
    }
    return {};
}
QString installArchive(const QString &archive, const QString &sha256, const QString &runtimesRoot, const QString &tag) {
    if (!parseTag(tag)) return "not a release tag: " + tag;
    if (sha256.isEmpty()) return "release asset has no SHA-256 digest";
    QString error;
    const QString actual = sha256Of(archive, &error);
    if (!error.isEmpty()) return error;
    if (actual.compare(sha256, Qt::CaseInsensitive) != 0) return "SHA-256 mismatch for " + archive + ": expected " + sha256 + ", got " + actual;
    const QString target = runtimesRoot + "/" + tag;
    if (QFileInfo::exists(target)) return target + " already exists";
    QProcess list;
    list.start("tar", {"--zstd", "--quoting-style=escape", "-tvf", archive});
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
    if (!QFileInfo(staging + "/usr/local/bin/darling").isFile())
        return "archive has no usr/local/bin/darling, partial files left in " + staging;
    if (!QDir().rename(staging, target)) return "cannot move " + staging + " to " + target;
    return {};
}
}
