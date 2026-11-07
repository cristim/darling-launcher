// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QList>
#include <QString>

// Per-prefix record of system libraries imported into the overlay upper layer
// (the prefix root itself). Stored in <prefix>/.darling-launcher/libraries.json.
struct LibrarySource {
    QString kind;        // fixture | cache | volume | cryptex
    QString cacheUUID;
    QString toolVersion;
    QString sha256;      // hex sha256 of the bytes imported at the install path
};
struct LibraryRecord {
    QString installName; // absolute guest path, e.g. /System/Library/PrivateFrameworks/Recap.framework/Versions/A/Recap
    QString id;          // first 16 hex of sha256(installName); names the store directory
    bool enabled = true;
    LibrarySource source;
};
struct LibrariesFile {
    bool enabled = true; // per-prefix master switch
    QList<LibraryRecord> libraries;
};

namespace Libraries {
constexpr int kMaxRecords = 4096;
QString idFor(const QString &installName);
bool validInstallName(const QString &installName, QString *error);
// Darling's own libraries (Cocotron AppKit/CoreGraphics/..., CoreFoundation, Foundation, libobjc, libSystem):
// imports must not shadow them unless forced.
bool isPinned(const QString &installName);
bool validRecord(const LibraryRecord &r, QString *error);
QString librariesPath(const QString &prefix);
QString storePath(const QString &prefix, const QString &id);
// A missing file is an empty default; anything unreadable or invalid fails closed.
bool load(const QString &prefix, LibrariesFile *out, QString *error);
// Validates and renders libraries.json; refuses content larger than load() would accept.
bool serialize(const LibrariesFile &file, QByteArray *json, QString *error);
bool save(const QString &prefix, const LibrariesFile &file, QString *error);
}
