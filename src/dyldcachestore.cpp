// SPDX-License-Identifier: GPL-3.0-or-later
#include "dyldcachestore.h"
#include "core.h"
#include "discovery.h"
#include "safefs.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSettings>
#include <QSet>
#include <cerrno>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace {
using LauncherCore::fail;
const qint64 kMaxManifestBytes = 1 << 20;
const QString kManifestName = QStringLiteral("store.json");
const QRegularExpression &hexRe(int n) {
    static const QRegularExpression h32("^[0-9a-f]{32}$"), h64("^[0-9a-f]{64}$");
    return n == 32 ? h32 : h64;
}
bool validFileName(const QString &name, const QString &arch) {
    const QString base = DyldCache::mainFileName(arch);
    return name == base || (name.startsWith(base) && DyldCache::validSuffix(name.mid(base.size()).toLatin1()));
}
}

namespace DyldCacheStore {
const char *const kGuestHostRoot = "/Volumes/SystemRoot";

QString defaultRoot() { return LauncherDiscovery::dataRoot() + "/dyld-cache"; }
QString configuredRoot() { return QSettings("cristim", "darling-launcher").value("dyldCacheRoot", defaultRoot()).toString(); }
QString storeId(const QString &arch, const QString &uuidHex) { return arch + "-" + uuidHex; }
bool validStoreId(const QString &id) {
    static const QRegularExpression re("^(arm64e?)-[0-9a-f]{32}$");
    return re.match(id).hasMatch();
}

namespace {
bool refuseSymlinks(const QString &root, const QString &dataRoot, QString *error) {
    const QFileInfo info(root);
    if (!info.isAbsolute() || root.contains(QChar(0))) return fail(error, "The store location must be an absolute path");
    const QString data = QDir::cleanPath(dataRoot);
    for (QString dir = QDir::cleanPath(root);;) {
        if (QFileInfo(dir).isSymLink()) return fail(error, "The store location, or a directory on the way to it inside the launcher data directory, is a symbolic link; choose a real directory");
        const QString up = QFileInfo(dir).absolutePath();
        if (up == dir || !up.startsWith(data + "/")) break;
        dir = up;
    }
    return true;
}
}

namespace {
// Missing directories on the way to the root are ours, so they are private too (mkpath would make them 0755).
bool makePrivateAncestors(const QString &dir) {
    const QFileInfo info(dir);
    if (info.exists()) return info.isDir();
    if (!makePrivateAncestors(info.absolutePath())) return false;
    return ::mkdir(QFile::encodeName(dir).constData(), 0700) == 0 || errno == EEXIST;
}
}

bool ensureRoot(const QString &root, QString *error, const QString &dataRoot) {
    if (!refuseSymlinks(root, dataRoot, error)) return false;
    const QFileInfo info(root);
    if (!makePrivateAncestors(info.absolutePath())) return fail(error, "Cannot create the parent of the store location");
    if (::mkdir(QFile::encodeName(root).constData(), 0700) != 0 && errno != EEXIST) return fail(error, "Cannot create the store location");
    return SafeFs::checkPrivateDir(info.absolutePath(), info.fileName(), error);
}

bool checkPrivate(const QString &root, const QString &id, QString *error, const QString &dataRoot) {
    if (!validStoreId(id)) return fail(error, "Invalid store id");
    if (!refuseSymlinks(root, dataRoot, error)) return false;
    const QFileInfo info(root);
    if (!SafeFs::checkPrivateDir(info.absolutePath(), info.fileName(), error)) return false;
    // a missing or symlinked store entry is status()'s to report
    return !QFileInfo(root + "/" + id).exists() || QFileInfo(root + "/" + id).isSymLink() || SafeFs::checkPrivateDir(root, id, error);
}

QByteArray renderManifest(const StoreManifest &m) {
    QJsonArray files;
    for (const auto &f : m.files) files.append(QJsonObject{{"name", f.name}, {"size", double(f.size)}, {"uuid", f.uuid}, {"sha256", f.sha256}});
    return QJsonDocument(QJsonObject{{"version", 1}, {"arch", m.arch}, {"uuid", m.uuid},
                                     {"copiedAt", m.copiedAt}, {"files", files}}).toJson(QJsonDocument::Indented);
}

bool parseManifest(const QByteArray &json, StoreManifest *out, QString *error) {
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(json, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) return fail(error, "store.json is not valid JSON");
    const auto o = doc.object();
    if (!o.value("version").isDouble() || o.value("version").toInt(-1) != 1) return fail(error, "Unsupported store.json version");
    for (const char *key : {"arch", "uuid", "copiedAt"})
        if (!o.value(key).isString()) return fail(error, QString("store.json field '%1' must be a string").arg(key));
    StoreManifest m;
    m.arch = o.value("arch").toString(); m.uuid = o.value("uuid").toString();
    m.copiedAt = o.value("copiedAt").toString().left(64);
    if (m.arch != "arm64" && m.arch != "arm64e") return fail(error, "store.json has an unsupported architecture");
    if (!hexRe(32).match(m.uuid).hasMatch()) return fail(error, "store.json has an invalid UUID");
    if (!o.value("files").isArray()) return fail(error, "store.json field 'files' must be an array");
    const auto files = o.value("files").toArray();
    if (files.isEmpty() || files.size() > DyldCache::kMaxFiles) return fail(error, "store.json lists an invalid number of files");
    QSet<QString> names;
    for (const auto &v : files) {
        const auto f = v.toObject();
        if (!v.isObject() || !f.value("name").isString() || !f.value("uuid").isString() || !f.value("sha256").isString() || !f.value("size").isDouble())
            return fail(error, "store.json file entry is malformed");
        StoreFile sf{f.value("name").toString(), qint64(f.value("size").toDouble(-1)), f.value("uuid").toString(), f.value("sha256").toString()};
        if (!validFileName(sf.name, m.arch) || names.contains(sf.name)) return fail(error, "store.json has an invalid or repeated file name");
        if (sf.size <= 0 || !hexRe(32).match(sf.uuid).hasMatch() || !hexRe(64).match(sf.sha256).hasMatch()) return fail(error, "store.json file entry has invalid fields");
        names.insert(sf.name); m.files.append(sf);
    }
    if (m.files.first().name != DyldCache::mainFileName(m.arch) || m.files.first().uuid != m.uuid) return fail(error, "store.json does not start with its main file");
    *out = m;
    return true;
}

bool saveManifest(const QString &root, const QString &id, const StoreManifest &m, QString *error) {
    if (!validStoreId(id)) return fail(error, "Invalid store id");
    return SafeFs::writeFile(root, id + "/" + kManifestName, renderManifest(m), 0400, 0700, true, error);
}

bool readHead(const QString &root, const QString &relPath, qint64 window, QByteArray *head, qint64 *fileSize, QString *error) {
    qint64 size = 0;
    const int fd = SafeFs::openRead(root, relPath, &size, error);
    if (fd < 0) return false;
    SafeFs::Fd guard(fd);
    QByteArray buf(int(qMin(window, size)), Qt::Uninitialized);
    if (!SafeFs::preadFull(fd, buf.data(), buf.size(), 0)) return fail(error, "Cannot read the start of " + QFileInfo(relPath).fileName());
    *head = buf; *fileSize = size;
    return true;
}

bool checkUsable(const QString &root, const QString &id, QString *error, const QString &dataRoot) {
    QString why;
    if (!checkPrivate(root, id, &why, dataRoot)) return fail(error, "The store cannot be trusted: " + why);
    const StoreStatus st = status(root, id);
    if (st.state != StoreState::Ready) return fail(error, "The store " + id + " is not ready" + (st.detail.isEmpty() ? QString() : " (" + st.detail + ")") + "; copy and verify it first");
    return true;
}

bool sourceId(const QString &sourceDir, const QString &arch, QString *id, QString *error) {
    QByteArray head; qint64 size = 0; DyldCache::MainHeader header;
    if (!readHead(sourceDir, DyldCache::mainFileName(arch), DyldCache::kHeaderWindow, &head, &size, error)
        || !DyldCache::parseMainHeader(head, size, &header, error)) return false;
    if (header.arch != arch) return fail(error, "The source cache is for " + header.arch + ", not " + arch);
    *id = storeId(arch, DyldCache::uuidHex(header.uuid));
    return true;
}

StoreStatus status(const QString &root, const QString &id) {
    StoreStatus st;
    if (!validStoreId(id)) { st.state = StoreState::Corrupt; st.detail = "Invalid store id"; return st; }
    const QFileInfo dir(root + "/" + id);
    if (!dir.exists() && !dir.isSymLink()) return st;
    QString err; QByteArray raw; bool exists = false;
    if (dir.isSymLink() || !dir.isDir()) { st.state = StoreState::Corrupt; st.detail = "The store entry is not a directory"; return st; }
    if (!SafeFs::readFile(root, id + "/" + kManifestName, kMaxManifestBytes, &raw, &exists, &err)) { st.state = StoreState::Corrupt; st.detail = "store.json is unreadable"; return st; }
    if (!exists) { st.state = StoreState::Partial; st.detail = "The copy is incomplete"; return st; }
    st.state = StoreState::Corrupt;
    if (!parseManifest(raw, &st.manifest, &err)) { st.detail = err; st.manifest = {}; return st; }
    if (DyldCacheStore::storeId(st.manifest.arch, st.manifest.uuid) != id) { st.detail = "store.json does not belong to this store"; return st; }
    for (const auto &f : st.manifest.files) {
        QByteArray head; qint64 size = 0;
        if (!readHead(root, id + "/" + f.name, 0x58 + 16, &head, &size, &err)) { st.detail = "Cannot read " + f.name; return st; }
        if (size != f.size) { st.detail = f.name + " has the wrong size"; return st; }
        QString arch; QByteArray uuid;
        if (!DyldCache::parseFileHeader(head, &arch, &uuid, &err) || arch != st.manifest.arch || DyldCache::uuidHex(uuid) != f.uuid) { st.detail = f.name + " does not match the manifest"; return st; }
    }
    st.state = StoreState::Ready;
    return st;
}

QStringList listStores(const QString &root) {
    QStringList ids;
    for (const auto &e : QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name))
        if (validStoreId(e.fileName())) ids << e.fileName();
    return ids;
}

qint64 freeBytes(const QString &existingDir) {
    struct statvfs sv;
    if (::statvfs(QFile::encodeName(existingDir).constData(), &sv) != 0) return -1;
    return qint64(sv.f_bavail) * qint64(sv.f_frsize);
}

bool checkFreeSpace(const QString &root, qint64 neededBytes, qint64 marginBytes, QString *error, const FreeBytesFn &fn) {
    QFileInfo probe(root);
    while (!probe.exists() && probe.absolutePath() != probe.absoluteFilePath()) probe = QFileInfo(probe.absolutePath());
    const qint64 free = fn(probe.absoluteFilePath());
    if (free < 0) return fail(error, "Cannot determine the free space at the store location");
    if (free < neededBytes + marginBytes)
        return fail(error, QString("Not enough free space: %1 MiB needed plus a %2 MiB margin, %3 MiB free")
                               .arg((neededBytes + (1 << 20) - 1) >> 20).arg(marginBytes >> 20).arg(free >> 20));
    return true;
}

bool guestDir(const QString &root, const QString &id, QString *guestPath, QString *error) {
    if (!validStoreId(id)) return fail(error, "Invalid store id");
    const QFileInfo dir(root + "/" + id);
    const QString canonical = dir.canonicalFilePath();
    if (canonical.isEmpty() || !QFileInfo(canonical).isDir()) return fail(error, "The store directory does not exist");
    for (const QChar c : canonical) if (c.unicode() < 0x20 || c.unicode() == 0x7f) return fail(error, "The store path contains a control character");
    const QString guest = QString::fromLatin1(kGuestHostRoot) + canonical;
    if (guest.toUtf8().size() > kMaxGuestDirChars)
        return fail(error, "The store location is too deep: the guest path would exceed " + QString::number(kMaxGuestDirChars) + " characters; choose a shorter location");
    *guestPath = guest;
    return true;
}
}
