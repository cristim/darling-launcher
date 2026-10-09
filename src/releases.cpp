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
