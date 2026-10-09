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

namespace LauncherReleases {
// Verifies archive against the release's SHA-256 digest, vets the member list (relative paths only, no
// device or hard-link members, nothing written through a symlink member), then extracts into
// <runtimesRoot>/<tag>. Absolute symlink targets are allowed because the runtime image uses them.
// Returns an empty string on success, otherwise the reason. An existing <tag> directory is an error.
QString installArchive(const QString &archive, const QString &sha256, const QString &runtimesRoot, const QString &tag);
// Lists tar --zstd members (type character + path + link target) and returns the reason the archive is unsafe, or "".
QString vetMembers(const QStringList &verboseListing);
}
