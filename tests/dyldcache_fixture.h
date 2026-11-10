// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic split-cache files: a header the launcher's parser accepts plus filler bytes. Never real cache data.
#pragma once
#include "dyldcacheheader.h"
#include <QDir>
#include <QFile>
#include <QtTest>
#include <cstring>

struct FakeSub { QString suffix; QByteArray uuid; qint64 size; };

static inline QByteArray fakeUuid(int seed) {
    QByteArray u(16, '\0');
    for (int i = 0; i < 16; ++i) u[i] = char(seed * 17 + i + 1);
    return u;
}
static inline void putLe32(QByteArray &b, int off, quint32 v) { std::memcpy(b.data() + off, &v, 4); }
// Where the real main file keeps its sub-cache table (measured), far beyond the first 64 KiB; early fixtures use 0x200.
static constexpr int kRealTableOffset = 0x392d8;
static inline QByteArray fakeFile(const QString &arch, const QByteArray &uuid, qint64 size, const QList<FakeSub> &subs, int fill, int tableOffset = 0x200) {
    QByteArray b(int(size), '\0');
    for (int i = 0; i < b.size(); ++i) b[i] = char((i * 31 + fill) & 0xff);
    const QByteArray magic = ("dyld_v1" + QByteArray(arch == "arm64e" ? 2 : 3, ' ') + arch.toLatin1());
    std::memset(b.data(), 0, 16); std::memcpy(b.data(), magic.constData(), size_t(qMin(16, int(magic.size()))));
    std::memcpy(b.data() + 0x58, uuid.constData(), 16);
    std::memset(b.data() + 0x188, 0, 8);
    if (!subs.isEmpty()) {
        putLe32(b, 0x188, quint32(tableOffset)); putLe32(b, 0x18c, quint32(subs.size()));
        for (int i = 0; i < subs.size(); ++i) {
            const int base = tableOffset + i * 56;
            std::memset(b.data() + base, 0, 56);
            std::memcpy(b.data() + base, subs[i].uuid.constData(), 16);
            std::memcpy(b.data() + base + 24, subs[i].suffix.toLatin1().constData(), size_t(subs[i].suffix.size()));
        }
    }
    return b;
}
// Writes dyld_shared_cache_<arch> plus one file per suffix into dir; returns the main uuid.
// The main file grows when the table at tableOffset would not fit.
static inline QByteArray makeFakeCache(const QString &dir, const QString &arch, int seed, const QStringList &suffixes, qint64 fileSize = 4096, int tableOffset = 0x200) {
    QDir().mkpath(dir);
    QList<FakeSub> subs;
    for (int i = 0; i < suffixes.size(); ++i) subs.append({suffixes[i], fakeUuid(seed * 100 + i + 1), fileSize});
    const QByteArray mainUuid = fakeUuid(seed * 100);
    auto write = [&](const QString &name, const QByteArray &data) { QFile f(dir + "/" + name); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(data); };
    write(DyldCache::mainFileName(arch), fakeFile(arch, mainUuid, qMax<qint64>(fileSize, tableOffset + 56 * subs.size()), subs, seed, tableOffset));
    for (int i = 0; i < subs.size(); ++i) write(DyldCache::mainFileName(arch) + subs[i].suffix, fakeFile(arch, subs[i].uuid, fileSize, {}, seed + i + 1));
    return mainUuid;
}

// A complete synthetic store <root>/<id>/ (files plus store.json). Returns the id.
#include "dyldcachestore.h"
static inline QString makeFakeStore(const QString &root, const QString &arch, int seed, const QStringList &suffixes, qint64 fileSize = 4096) {
    const QByteArray uuid = fakeUuid(seed * 100);
    const QString id = DyldCacheStore::storeId(arch, DyldCache::uuidHex(uuid));
    makeFakeCache(root + "/" + id, arch, seed, suffixes, fileSize);
    QDir().mkpath(root);
    StoreManifest m; m.arch = arch; m.uuid = DyldCache::uuidHex(uuid); m.copiedAt = "2026-01-01T00:00:00Z";
    const QString shaz(64, 'a');
    m.files.append({DyldCache::mainFileName(arch), fileSize, m.uuid, shaz});
    for (int i = 0; i < suffixes.size(); ++i) m.files.append({DyldCache::mainFileName(arch) + suffixes[i], fileSize, DyldCache::uuidHex(fakeUuid(seed * 100 + i + 1)), shaz});
    QFile::setPermissions(root + "/" + id, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner); // as the real copy makes it
    QFile f(root + "/" + id + "/store.json");
    if (!f.open(QIODevice::WriteOnly)) return {};
    f.write(DyldCacheStore::renderManifest(m));
    return id;
}
