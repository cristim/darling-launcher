// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QList>
#include <QString>

// The parts of a macOS dyld shared cache file header the launcher needs: which cache this is (arch, UUID) and which
// sub-cache files belong to it. Offsets mirror dyld_cache_header / dyld_subcache_entry in Darling's dyld
// (dyld3/shared-cache/dyld_cache_format.h). The bytes come from a user-supplied volume, so every field is checked.
namespace DyldCache {
constexpr int kMaxFiles = 64;           // main file plus sub-caches
constexpr qint64 kHeaderWindow = 64 * 1024; // bytes read from the start of the main file; the sub-cache table must lie inside
constexpr int kUuidBytes = 16;

struct SubCache {
    QString suffix;    // e.g. ".01"; the file name is the main file's name plus this
    QByteArray uuid;   // must equal the UUID in the sub-cache file's own header
};
struct MainHeader {
    QString arch;      // arm64 or arm64e
    QByteArray uuid;
    QList<SubCache> subCaches;
};

QString mainFileName(const QString &arch);
// Lowercase hex of the UUID bytes (32 characters).
QString uuidHex(const QByteArray &uuid);
// A plain file suffix such as ".01" or ".02.dylddata": at most 31 bytes, safe characters, no "..".
bool validSuffix(const QByteArray &suffix);
// Parses the first bytes of a main file. `fileSize` bounds what the header may point at.
bool parseMainHeader(const QByteArray &head, qint64 fileSize, MainHeader *out, QString *error);
// Magic and UUID of any cache file (main or sub-cache). *arch is the architecture named by the magic.
bool parseFileHeader(const QByteArray &head, QString *arch, QByteArray *uuid, QString *error);
}
