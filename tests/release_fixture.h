// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "releases.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>

// A fake `gh` serving a CI-style runtime release (release list, manifest.json, archive, `attestation verify`) from
// <root>/fake. The archive's launcher is a fake `darling` that passes LauncherCore::checkPrefix unless
// <prefix>/../fail-init, fail-uname or fail-ls exists (each makes the matching step fail). Every gh call is appended to <root>/fake/calls.
struct FakeRelease {
    QString gh, fake, file, sha, tag = "v2026.10.09-3721b65", repo = "VibeDarling/darling", arch = LauncherReleases::hostArchitecture();

    static bool write(const QString &path, const QByteArray &data, bool executable = false) {
        QFile output(path);
        if (!output.open(QIODevice::WriteOnly)) return false;
        output.write(data); output.close();
        return !executable || output.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    QJsonObject asset(const QString &name, const QString &digest = {}) const {
        QJsonObject object{{"name", name}, {"browser_download_url", "https://github.com/" + repo + "/releases/download/x/" + name}};
        if (!digest.isEmpty()) object.insert("digest", digest);
        return object;
    }
    bool create(const QString &root) {
        fake = root + "/fake"; gh = root + "/gh"; file = "darling-runtime-" + tag + "-linux-" + arch + ".tar.zst";
        if (!QDir().mkpath(fake + "/assets") || !QDir().mkpath(root + "/image/usr/local/bin") || !QDir().mkpath(root + "/image/usr/local/libexec/darling/private/etc")) return false;
        if (!write(root + "/image/usr/local/bin/darling",
                   "#!/bin/sh\ncase \"$1\" in\nshell) if [ -e \"$DPREFIX/../fail-init\" ]; then echo init-broken >&2; exit 1; fi; mkdir -p \"$DPREFIX/private/etc\"; exit 0;;\nexec) shift; cmd=$1; shift; case \"$cmd\" in /bin/echo) echo \"$@\";; "
                   "/usr/bin/uname) if [ -e \"$DPREFIX/../fail-uname\" ]; then echo Linux; else echo Darwin; fi;; /bin/ls) if [ -e \"$DPREFIX/../fail-ls\" ]; then exit 1; fi; echo \"$1\";; esac;;\nesac\nexit 0\n", true)) return false;
        QProcess tar; tar.start("tar", {"--zstd", "-cf", fake + "/assets/" + file, "-C", root + "/image", "usr"});
        if (!tar.waitForFinished(30000) || tar.exitCode() != 0) return false;
        QFile archive(fake + "/assets/" + file);
        if (!archive.open(QIODevice::ReadOnly)) return false;
        sha = QCryptographicHash::hash(archive.readAll(), QCryptographicHash::Sha256).toHex();
        return write(gh, ("#!/bin/sh\nF='" + fake + "'\necho \"$@\" >> \"$F/calls\"\ncase \"$1 $2\" in\n"
            "\"api repos/" + repo + "/releases?per_page=100\") cat \"$F/release.json\";;\n"
            "\"release download\") shift 2; while [ $# -gt 0 ]; do case $1 in --pattern) p=$2; shift;; --dir) d=$2; shift;; esac; shift; done; cp \"$F/assets/$p\" \"$d/$p\" || exit 1;;\n"
            "\"attestation verify\") ;;\n*) echo \"unexpected: $*\" >&2; exit 2;;\nesac\n").toUtf8(), true) && publish(sha);
    }
    // The list has a newer changelog release, prerelease and draft (which must be skipped) and, if `runtime`, `tag` with this architecture's artifact.
    bool publish(const QString &manifestSha, bool runtime = true, const QJsonArray &sonames = {}, const QString &glibc = "2.0") const {
        const QString url = "https://github.com/" + repo + "/releases/download/" + tag + "/" + file;
        const QJsonObject entry{{"file", file}, {"url", url}, {"sha256", manifestSha}, {"size", 1}, {"unpacked_size", 2}, {"install_root", "usr/local"}, {"launcher", "usr/local/bin/darling"},
                                {"host_requires", QJsonObject{{"glibc", glibc}, {"wrapped_sonames", sonames}}}};
        QJsonArray releases{
            QJsonObject{{"tag_name", "v0.1.20260921"}, {"created_at", "2026-10-12T00:00:00Z"}, {"assets", QJsonArray{}}},
            QJsonObject{{"tag_name", "v2026.10.11-bbbbbbb"}, {"created_at", "2026-10-11T00:00:00Z"}, {"prerelease", true}, {"assets", QJsonArray{asset("manifest.json")}}},
            QJsonObject{{"tag_name", "v2026.10.10-ccccccc"}, {"created_at", "2026-10-10T00:00:00Z"}, {"draft", true}, {"assets", QJsonArray{asset("manifest.json")}}}};
        if (runtime) releases.append(QJsonObject{{"tag_name", tag}, {"created_at", "2026-10-09T00:00:00Z"}, {"assets", QJsonArray{asset("manifest.json"), QJsonObject{{"name", file}, {"browser_download_url", url}, {"digest", "sha256:" + manifestSha}}}}});
        return write(fake + "/assets/manifest.json", QJsonDocument(QJsonObject{{"schema", 1}, {"version", tag}, {"artifacts", QJsonObject{{arch, entry}}}}).toJson())
            && write(fake + "/release.json", QJsonDocument(releases).toJson());
    }
    QString calls() const { QFile f(fake + "/calls"); return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString(); }
    void clearCalls() const { QFile::remove(fake + "/calls"); }
};
