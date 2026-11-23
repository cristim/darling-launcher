// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
namespace LauncherReleases {
// The CI release contract: tag vYYYY.MM.DD-<sha7>[-rN], manifest.json with artifacts keyed by `uname -m`.
struct Artifact { QString file, url, sha256, installRoot, launcher; qint64 size = 0, unpackedSize = 0; };
struct Selection { QString tag; Artifact artifact; QString error; bool valid() const { return error.isEmpty(); } };
constexpr const char *installRoot = "usr/local";
constexpr const char *launcherPath = "usr/local/bin/darling";
// Also guards runtime directory names: only tags pass.
bool validTag(const QString &tag);
// `uname -m` (aarch64, x86_64); the manifest keys are never aliased.
QString hostArchitecture();
// Selects this architecture's artifact from a releases/latest document and its manifest.json. Drafts,
// prereleases (unless allowPrerelease), unknown schemas, a missing architecture, a manifest that
// disagrees with the release or the API digest, and install_root/launcher other than the fixed values are errors.
Selection select(const QJsonObject &release, const QJsonObject &manifest, const QString &arch, bool allowPrerelease);
// Runs `gh attestation verify`, pinned to repo's release-binaries.yml workflow. Empty string = verified.
QString verifyAttestation(const QString &gh, const QString &archive, const QString &repo);
// Verifies the digest and attestation, vets the member list (relative paths only, no device or
// hard-link members, nothing written through a symlink, a regular usr/local/bin/darling), then extracts
// into <runtimesRoot>/<tag>. Absolute symlink targets are allowed because the runtime image uses them.
// Returns an empty string on success, otherwise the reason.
QString installArchive(const QString &archive, const QString &sha256, const QString &runtimesRoot, const QString &tag, const QString &gh, const QString &repo);
// The newest (created_at) stable release in a releases listing whose tag passes validTag and that has a
// manifest.json asset, as CI's lock_diff.py select_release picks it; {} if none. Other releases in the same
// repo (v0.1.* changelogs) must not shadow the runtime release, which is why /releases/latest is not used.
QJsonObject latestRuntimeRelease(const QJsonArray &releases);
// Fetches repo's newest runtime release with `gh`, selects this architecture's artifact from its manifest.json,
// downloads it into a temporary folder under runtimesRoot and runs installArchive. Blocking (up to
// ~30 min); call it off the GUI thread. A runtime already installed for the latest tag is not downloaded again.
// Returns an empty string on success (and sets *installedTag if given), otherwise the reason.
QString installLatest(const QString &gh, const QString &repo, const QString &runtimesRoot, QString *installedTag = nullptr);
// Takes `tar --zstd --quoting-style=escape -tv` lines; returns why the archive is unsafe, or "".
QString vetMembers(const QStringList &verboseListing);
}
