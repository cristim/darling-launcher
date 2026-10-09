// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDateTime>
#include <QJsonArray>
#include <QList>
#include <QString>
#include <optional>
namespace LauncherReleases {
struct Tag { QDateTime time; QString sha; };
struct Asset { QString name, url, sha256; };
struct Release { QString tag; QDateTime time, published; QString sha; QList<Asset> assets; };
struct Selection { Release release; Asset asset; QString error; bool valid() const { return error.isEmpty(); } };
// Release tags are YYYY-MM-DD-HH-MM-<sha> in UTC.
std::optional<Tag> parseTag(const QString &tag);
QString assetName(const QString &component, const QString &arch);
QString hostArchitecture();
// Picks the newest release (by tag time, then publication time) from a GitHub releases listing and its
// darling-<component>-<arch>.tar.zst asset. Drafts and unparsable tags are skipped; a newest release
// without the asset or its SHA-256 digest, or two newest releases that cannot be ordered, is an error.
Selection latest(const QJsonArray &releases, const QString &component, const QString &arch);
}
