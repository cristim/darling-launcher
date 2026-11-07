// SPDX-License-Identifier: GPL-3.0-or-later
#include "libraries.h"
#include "core.h"
#include "safefs.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

namespace {
const char *const kSourceKinds[] = {"fixture", "cache", "volume", "cryptex"};
const char *const kAllowedRoots[] = {"System/Library/", "usr/lib/", "Library/Frameworks/"};
// Frameworks Darling ships itself (Cocotron plus Apple-source CoreFoundation/Foundation).
// Everything else Darling's runtime provides is protected by the "builtin" check at import time
// (a path the runtime already has is refused unless forced), so this list is not the only guard.
const char *const kPinnedFrameworks[] = {"AppKit", "Cocoa", "CoreData", "CoreGraphics", "CoreText",
                                         "QuartzCore", "CoreFoundation", "Foundation"};
const char *const kPinnedLibPrefixes[] = {"/usr/lib/libobjc", "/usr/lib/libSystem"};
// Limits on guest-writable input.
const qint64 kMaxFileBytes = 1 << 20;
const QString kRelPath = QStringLiteral(".darling-launcher/libraries.json");
const int kMaxNameBytes = 4096;
const int kMaxComponentBytes = 255;
const int kMaxFieldChars = 128;
const QRegularExpression &sha256Pattern() { static const QRegularExpression re("^[0-9a-f]{64}$"); return re; }
bool validKind(const QString &kind) {
    for (const char *k : kSourceKinds) if (kind == k) return true;
    return false;
}
}

namespace Libraries {
QString idFor(const QString &installName) {
    return QString::fromLatin1(QCryptographicHash::hash(installName.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
}

bool validInstallName(const QString &installName, QString *error) {
    using LauncherCore::fail;
    if (installName.contains(QChar(0))) return fail(error, "Install name contains a NUL");
    if (!installName.startsWith('/')) return fail(error, "Install name is not absolute (@rpath-style names are not importable): " + installName);
    const QString relative = installName.mid(1);
    if (installName.toUtf8().size() > kMaxNameBytes) return fail(error, "Install name is too long");
    if (!LauncherCore::confinedRelative(relative)) return fail(error, "Install name is not confined: " + installName);
    for (const auto &part : relative.split('/'))
        if (part.toUtf8().size() > kMaxComponentBytes) return fail(error, "Install name has an over-long component");
    bool allowed = false;
    for (const char *root : kAllowedRoots) allowed = allowed || relative.startsWith(QLatin1String(root));
    if (!allowed) return fail(error, "Install name is outside the supported macOS system locations: " + installName);
    return true;
}

bool isPinned(const QString &installName) {
    for (const char *name : kPinnedFrameworks)
        if (installName.startsWith("/System/Library/Frameworks/" + QString::fromLatin1(name) + ".framework/")) return true;
    for (const char *prefix : kPinnedLibPrefixes)
        if (installName.startsWith(QLatin1String(prefix))) return true;
    return false;
}

QString librariesPath(const QString &prefix) { return prefix + "/" + kRelPath; }
QString storePath(const QString &prefix, const QString &id) { return prefix + "/.darling-launcher/libs/" + id; }

bool validRecord(const LibraryRecord &r, QString *error) {
    using LauncherCore::fail;
    if (!validInstallName(r.installName, error)) return false;
    if (r.id != idFor(r.installName)) return fail(error, "Record id does not match its install name: " + r.installName);
    if (!validKind(r.source.kind)) return fail(error, "Unknown source kind '" + r.source.kind + "'");
    if (!sha256Pattern().match(r.source.sha256).hasMatch()) return fail(error, "Record has no valid sha256: " + r.installName);
    if (r.source.cacheUUID.size() > kMaxFieldChars || r.source.toolVersion.size() > kMaxFieldChars)
        return fail(error, "Record source field is too long: " + r.installName);
    return true;
}

// Fail closed on missing or wrongly typed fields: the file is guest-writable.
static bool strictBool(const QJsonObject &o, const char *key, bool *out, QString *error) {
    const auto v = o.value(key);
    if (!v.isBool()) return LauncherCore::fail(error, QString("libraries.json field '%1' must be a boolean").arg(key));
    *out = v.toBool();
    return true;
}
static bool strictString(const QJsonObject &o, const char *key, QString *out, QString *error) {
    const auto v = o.value(key);
    if (!v.isString()) return LauncherCore::fail(error, QString("libraries.json field '%1' must be a string").arg(key));
    *out = v.toString();
    return true;
}

bool load(const QString &prefix, LibrariesFile *out, QString *error) {
    using LauncherCore::fail;
    *out = {};
    // guest-writable path: read through a descriptor chain that refuses symlinks, FIFOs and oversized files
    QByteArray raw; bool exists = false;
    if (!SafeFs::readFile(prefix, kRelPath, kMaxFileBytes, &raw, &exists, error)) return false;
    if (!exists) return true;
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(raw, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) return fail(error, "libraries.json is not valid JSON");
    const auto root = doc.object();
    const auto version = root.value("version");
    if (!version.isDouble() || version.toInt(-1) != 1) return fail(error, "Unsupported libraries.json version");
    LibrariesFile result;
    if (!strictBool(root, "enabled", &result.enabled, error)) return false;
    if (!root.value("libraries").isArray()) return fail(error, "libraries.json field 'libraries' must be an array");
    const auto array = root.value("libraries").toArray();
    if (array.size() > kMaxRecords) return fail(error, "libraries.json has too many records");
    QSet<QString> names, ids;
    for (const auto &value : array) {
        if (!value.isObject() || !value.toObject().value("source").isObject()) return fail(error, "libraries.json record is malformed");
        const auto o = value.toObject(); const auto s = o.value("source").toObject();
        LibraryRecord r;
        if (!strictString(o, "installName", &r.installName, error) || !strictString(o, "id", &r.id, error) ||
            !strictBool(o, "enabled", &r.enabled, error) || !strictString(s, "kind", &r.source.kind, error) ||
            !strictString(s, "cacheUUID", &r.source.cacheUUID, error) || !strictString(s, "toolVersion", &r.source.toolVersion, error) ||
            !strictString(s, "sha256", &r.source.sha256, error)) return false;
        if (!validRecord(r, error)) return false;
        if (names.contains(r.installName) || ids.contains(r.id)) return fail(error, "Duplicate record: " + r.installName);
        names.insert(r.installName); ids.insert(r.id);
        result.libraries.append(r);
    }
    *out = result;
    return true;
}

bool serialize(const LibrariesFile &file, QByteArray *json, QString *error) {
    QJsonArray records; QSet<QString> names, ids;
    if (file.libraries.size() > kMaxRecords) return LauncherCore::fail(error, "Too many records");
    for (const auto &r : file.libraries) {
        if (!validRecord(r, error)) return false;
        if (names.contains(r.installName) || ids.contains(r.id)) return LauncherCore::fail(error, "Duplicate record: " + r.installName);
        names.insert(r.installName); ids.insert(r.id);
        records.append(QJsonObject{{"installName", r.installName}, {"id", r.id}, {"enabled", r.enabled},
            {"source", QJsonObject{{"kind", r.source.kind}, {"cacheUUID", r.source.cacheUUID}, {"toolVersion", r.source.toolVersion}, {"sha256", r.source.sha256}}}});
    }
    *json = QJsonDocument(QJsonObject{{"version", 1}, {"enabled", file.enabled}, {"libraries", records}}).toJson(QJsonDocument::Indented);
    // load() refuses larger files, so never write one
    if (json->size() > kMaxFileBytes) return LauncherCore::fail(error, "libraries.json would exceed its size limit");
    return true;
}

bool save(const QString &prefix, const LibrariesFile &file, QString *error) {
    QByteArray json;
    if (!serialize(file, &json, error)) return false;
    return SafeFs::writeFile(prefix, kRelPath, json, 0644, 0755, true, error);
}
}
