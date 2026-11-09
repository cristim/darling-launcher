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
static inline void put32(QByteArray &b, int off, quint32 v) { std::memcpy(b.data() + off, &v, 4); }
static inline QByteArray fakeFile(const QString &arch, const QByteArray &uuid, qint64 size, const QList<FakeSub> &subs, int fill) {
    QByteArray b(int(size), '\0');
    for (int i = 0; i < b.size(); ++i) b[i] = char((i * 31 + fill) & 0xff);
    const QByteArray magic = ("dyld_v1" + QByteArray(arch == "arm64e" ? 2 : 3, ' ') + arch.toLatin1());
    std::memset(b.data(), 0, 16); std::memcpy(b.data(), magic.constData(), size_t(qMin(16, int(magic.size()))));
    std::memcpy(b.data() + 0x58, uuid.constData(), 16);
    std::memset(b.data() + 0x188, 0, 8);
    if (!subs.isEmpty()) {
        put32(b, 0x188, 0x200); put32(b, 0x18c, quint32(subs.size()));
        for (int i = 0; i < subs.size(); ++i) {
            const int base = 0x200 + i * 56;
            std::memset(b.data() + base, 0, 56);
            std::memcpy(b.data() + base, subs[i].uuid.constData(), 16);
            std::memcpy(b.data() + base + 24, subs[i].suffix.toLatin1().constData(), size_t(subs[i].suffix.size()));
        }
    }
    return b;
}
// Writes dyld_shared_cache_<arch> plus one file per suffix into dir; returns the main uuid.
static inline QByteArray makeFakeCache(const QString &dir, const QString &arch, int seed, const QStringList &suffixes, qint64 fileSize = 4096) {
    QDir().mkpath(dir);
    QList<FakeSub> subs;
    for (int i = 0; i < suffixes.size(); ++i) subs.append({suffixes[i], fakeUuid(seed * 100 + i + 1), fileSize});
    const QByteArray mainUuid = fakeUuid(seed * 100);
    auto write = [&](const QString &name, const QByteArray &data) { QFile f(dir + "/" + name); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(data); };
    write(DyldCache::mainFileName(arch), fakeFile(arch, mainUuid, fileSize, subs, seed));
    for (int i = 0; i < subs.size(); ++i) write(DyldCache::mainFileName(arch) + subs[i].suffix, fakeFile(arch, subs[i].uuid, fileSize, {}, seed + i + 1));
    return mainUuid;
}
