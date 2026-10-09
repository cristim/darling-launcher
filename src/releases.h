// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>
namespace LauncherReleases {
// The CI release contract: tag vYYYY.MM.DD-<sha7>[-rN], manifest.json with artifacts keyed by `uname -m`.
struct Artifact { QString file, url, sha256, installRoot, launcher; qint64 size = 0, unpackedSize = 0; };
struct Selection { QString tag; Artifact artifact; QString error; bool valid() const { return error.isEmpty(); } };
constexpr const char *installRoot = "usr/local";
constexpr const char *launcherPath = "usr/local/bin/darling";
constexpr const char *runtimeMarker = "usr/local/libexec/darling/private/etc";
// Also guards runtime directory names: only tags pass.
bool validTag(const QString &tag);
// `uname -m` (aarch64, x86_64); the manifest keys are never aliased.
QString hostArchitecture();
// Selects this architecture's artifact from a releases/latest document and its manifest.json. Drafts,
// prereleases (unless allowPrerelease), unknown schemas, a missing architecture, a manifest that
// disagrees with the release or the API digest, and install_root/launcher other than the fixed values are errors.
Selection select(const QJsonObject &release, const QJsonObject &manifest, const QString &arch, bool allowPrerelease);
// Trust root for prebuilt runtimes: only builds attested by this workflow install, whatever repo they are
// fetched from (DESIGN sec 6). Accepting a fork's own workflow must be an explicit, user-visible exception.
constexpr const char *signerWorkflow = "VibeDarling/darling/.github/workflows/release-binaries.yml";
// Only master builds count: a workflow edited on another branch can sign anything. Prereleases dispatched from
// another ref (DESIGN sec 5) therefore fail verification even with allowPrerelease; accepting one needs the
// same explicit, user-visible exception as a fork.
constexpr const char *signerSourceRef = "refs/heads/master";
// Runs `gh attestation verify <archive> --repo <repo> --signer-workflow <signerWorkflow> --source-ref <signerSourceRef>
// --deny-self-hosted-runners`. Empty string = verified.
QString verifyAttestation(const QString &gh, const QString &archive, const QString &repo);
// Verifies the digest and attestation, vets the member list (relative paths under usr/local only, no device or
// hard-link members, nothing written through a symlink, no symlink at usr/local/bin/darling, the private/etc
// marker or any of their parents), then extracts and checks extractedTreeError before moving it
// into <runtimesRoot>/<tag>. Absolute and ".." symlink targets are allowed because the runtime image uses them.
// Extraction goes to <tag>.partial, created with an exclusive mkdir, so one install of a tag runs at a time;
// a failed install removes the .partial it created and never one that already existed.
// Returns an empty string on success, otherwise the reason.
QString installArchive(const QString &archive, const QString &sha256, const QString &runtimesRoot, const QString &tag, const QString &gh, const QString &repo);
// Why <dir> is not a self-contained runtime, or "": usr/local, the launcher (a regular file) and the
// private/etc marker must resolve to themselves inside <dir>, so no symlinked component can point elsewhere.
QString runtimeLayoutError(const QString &dir);
// Re-checks what tar actually wrote, independent of the listing: only usr/local/**, only regular files with
// one link and no setuid/setgid bit, folders with u+rwx, and symlinks, then runtimeLayoutError.
QString extractedTreeError(const QString &dir);
// Takes `tar --zstd --quoting-style=escape --numeric-owner -tv` lines; returns why the archive is unsafe, or "".
// A line that does not parse (including a non-numeric owner) is refused.
QString vetMembers(const QStringList &verboseListing);
}
