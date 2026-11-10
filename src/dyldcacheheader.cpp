// SPDX-License-Identifier: GPL-3.0-or-later
#include "dyldcacheheader.h"
#include "core.h"
#include <QRegularExpression>
#include <QSet>
#include <cstring>

namespace {
using LauncherCore::fail;
constexpr int kMagicBytes = 16;
constexpr int kUuidOffset = 0x58;
constexpr int kSubCacheArrayOffsetField = 0x188;
constexpr int kSubCacheArrayCountField = 0x18c;
constexpr int kFirstUsableTableOffset = 0x190; // the table cannot overlap the fields that locate it
constexpr int kSubCacheEntryBytes = 56;
constexpr int kSubCacheSuffixOffset = 24;
constexpr int kSuffixBytes = 32;

quint32 u32(const QByteArray &b, int off) {
    quint32 v; std::memcpy(&v, b.constData() + off, 4); return v; // little-endian host, like the cache
}
}

namespace DyldCache {
bool validSuffix(const QByteArray &s) {
    static const QRegularExpression re("^\\.[A-Za-z0-9._-]+$");
    return !s.isEmpty() && s.size() < kSuffixBytes && re.match(QString::fromLatin1(s)).hasMatch() && !s.contains("..");
}

QString mainFileName(const QString &arch) { return "dyld_shared_cache_" + arch; }
QString uuidHex(const QByteArray &uuid) { return QString::fromLatin1(uuid.toHex()); }

bool parseFileHeader(const QByteArray &head, QString *arch, QByteArray *uuid, QString *error) {
    if (head.size() < kUuidOffset + kUuidBytes) return fail(error, "Cache file is too short to hold a header");
    const QByteArray magic = head.left(kMagicBytes);
    if (!magic.startsWith("dyld_v1")) return fail(error, "Not a split dyld cache file (bad magic)");
    QByteArray name = magic.mid(7);
    const int nul = name.indexOf('\0');
    if (nul >= 0) name = name.left(nul);
    name = name.trimmed();
    if (name != "arm64" && name != "arm64e") return fail(error, "Unsupported cache architecture '" + QString::fromLatin1(name.left(16)) + "'");
    const QByteArray id = head.mid(kUuidOffset, kUuidBytes);
    if (id == QByteArray(kUuidBytes, '\0')) return fail(error, "Cache file has an all-zero UUID");
    *arch = QString::fromLatin1(name); *uuid = id;
    return true;
}

bool parseMainHeader(const QByteArray &head, qint64 fileSize, MainHeader *out, QString *error) {
    MainHeader h;
    if (!parseFileHeader(head, &h.arch, &h.uuid, error)) return false;
    if (head.size() < kSubCacheArrayCountField + 4) return fail(error, "Cache header is truncated");
    const quint32 offset = u32(head, kSubCacheArrayOffsetField), count = u32(head, kSubCacheArrayCountField);
    if (count > quint32(kMaxFiles - 1)) return fail(error, "Cache lists too many sub-cache files");
    if (count != 0) {
        const qint64 end = qint64(offset) + qint64(count) * kSubCacheEntryBytes;
        if (offset < quint32(kFirstUsableTableOffset) || offset % 8 != 0) return fail(error, "Sub-cache table offset is invalid");
        if (end > fileSize || end > head.size()) return fail(error, "Sub-cache table lies outside the cache file header");
    }
    QSet<QString> seen;
    for (quint32 i = 0; i != count; ++i) {
        const int base = int(offset + i * kSubCacheEntryBytes);
        QByteArray suffix = head.mid(base + kSubCacheSuffixOffset, kSuffixBytes);
        const int nul = suffix.indexOf('\0');
        if (nul < 0) return fail(error, "Sub-cache suffix is not terminated");
        suffix = suffix.left(nul);
        if (!validSuffix(suffix)) return fail(error, "Sub-cache suffix is not a plain file suffix");
        const QString s = QString::fromLatin1(suffix);
        if (seen.contains(s)) return fail(error, "Sub-cache suffix is listed twice: " + s);
        seen.insert(s);
        h.subCaches.append({s, head.mid(base, kUuidBytes)});
    }
    *out = h;
    return true;
}
}
