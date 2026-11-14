// SPDX-License-Identifier: GPL-3.0-or-later
#include "macho.h"
#include "core.h"
#include <QByteArrayView>
#include <QList>
#include <QSet>
#include <algorithm>

namespace {
using LauncherCore::fail;
const quint32 kMhMagic64 = 0xfeedfacf;
const quint32 kFatMagic = 0xcafebabe;
const quint32 kCpuArm64 = 0x0100000c;
const quint32 kSubtypeMask = 0x00ffffff;
const quint64 kHeaderSize = 32;
const quint64 kLoadCmdMin = 8;
const quint64 kDylibCmdMin = 24;
const quint32 kMaxNcmds = 2048;
const quint32 kMaxSizeofcmds = 1u << 20;
const quint32 kMaxFatArch = 16;
const quint32 kMaxNameBytes = 1024;
const int kMaxNodes = 256;
const int kMaxDepth = 32;
// LOAD_DYLIB, LOAD_WEAK_DYLIB, REEXPORT_DYLIB, LAZY_LOAD_DYLIB, LOAD_UPWARD_DYLIB
const quint32 kDylibCmds[] = {0xc, 0x80000018, 0x8000001f, 0x20, 0x80000023};
const quint32 kLoadWeakDylib = 0x80000018;
const quint32 kFatMagic64 = 0xcafebabf, kFatCigam64 = 0xbfbafeca;

bool u32(QByteArrayView b, quint64 off, bool bigEndian, quint32 *v) {
    if (off + 4 > quint64(b.size())) return false;
    const auto *p = reinterpret_cast<const uchar *>(b.constData()) + off;
    *v = bigEndian ? (quint32(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3])
                   : (quint32(p[3]) << 24 | p[2] << 16 | p[1] << 8 | p[0]);
    return true;
}
bool isDylibCmd(quint32 cmd) { return std::find(std::begin(kDylibCmds), std::end(kDylibCmds), cmd) != std::end(kDylibCmds); }

struct Slice { QByteArrayView bytes; quint32 subtype; };

bool readThin(QByteArrayView s, QList<MachO::Dependency> *names, quint32 *subtype, QString *error) {
    quint32 magic, cpu, sub, ncmds, sizeofcmds;
    if (!u32(s, 0, false, &magic) || !u32(s, 4, false, &cpu) || !u32(s, 8, false, &sub) ||
        !u32(s, 16, false, &ncmds) || !u32(s, 20, false, &sizeofcmds))
        return fail(error, "Mach-O header is truncated");
    if (magic != kMhMagic64) return fail(error, "Not a little-endian 64-bit Mach-O image");
    if (cpu != kCpuArm64) return fail(error, "Mach-O image is not arm64");
    *subtype = sub & kSubtypeMask;
    if (ncmds > kMaxNcmds) return fail(error, "Mach-O has too many load commands");
    if (sizeofcmds > kMaxSizeofcmds) return fail(error, "Mach-O load commands are too large");
    if (kHeaderSize + sizeofcmds > quint64(s.size())) return fail(error, "Mach-O load commands run past the end of the file");
    quint64 pos = kHeaderSize;
    const quint64 end = kHeaderSize + sizeofcmds;
    for (quint32 i = 0; i < ncmds; ++i) {
        quint32 cmd, cmdsize;
        if (!u32(s, pos, false, &cmd) || !u32(s, pos + 4, false, &cmdsize)) return fail(error, "Load command header is truncated");
        if (cmdsize < kLoadCmdMin) return fail(error, "Load command is smaller than its header");
        if (cmdsize % 8 != 0) return fail(error, "Load command size is not a multiple of 8");
        if (pos + cmdsize > end) return fail(error, "Load command runs past sizeofcmds");
        if (isDylibCmd(cmd)) {
            quint32 nameOff;
            if (cmdsize < kDylibCmdMin || !u32(s, pos + 8, false, &nameOff)) return fail(error, "Dylib command is too small");
            if (nameOff < kDylibCmdMin || nameOff >= cmdsize) return fail(error, "Dylib name offset is outside its command");
            const QByteArrayView nameArea = s.mid(pos + nameOff, cmdsize - nameOff);
            const qsizetype len = nameArea.indexOf('\0');
            if (len < 0) return fail(error, "Dylib name is not NUL-terminated within its command");
            if (len > qsizetype(kMaxNameBytes)) return fail(error, "Dylib name is too long");
            names->append({QString::fromUtf8(nameArea.constData(), len), cmd == kLoadWeakDylib});
        }
        pos += cmdsize;
    }
    return true;
}

bool pickSlice(const QByteArray &image, quint32 preferred, bool allowFallback, Slice *out, bool *fallback, QString *error) {
    const QByteArrayView all(image);
    quint32 magic;
    if (!u32(all, 0, true, &magic)) return fail(error, "Mach-O file is too short");
    if (magic == kFatMagic64 || magic == kFatCigam64) return fail(error, "64-bit fat Mach-O (FAT_MAGIC_64) is not supported");
    if (magic != kFatMagic) {
        quint32 sub, cpu;
        if (!u32(all, 4, false, &cpu) || !u32(all, 8, false, &sub)) return fail(error, "Mach-O header is truncated");
        *out = {all, sub & kSubtypeMask};
        *fallback = (*out).subtype != preferred;
        if (cpu == kCpuArm64 && *fallback && !allowFallback) return fail(error, "Image has no slice for the requested arm64 subtype");
        return true;
    }
    quint32 n;
    if (!u32(all, 4, true, &n)) return fail(error, "Fat header is truncated");
    if (n == 0 || n > kMaxFatArch) return fail(error, "Fat header has an invalid arch count");
    struct Arch { quint32 cpu, sub, offset, size; };
    QList<Arch> arches;
    for (quint32 i = 0; i < n; ++i) {
        Arch a;
        const quint64 p = 8 + 20ull * i;
        if (!u32(all, p, true, &a.cpu) || !u32(all, p + 4, true, &a.sub) || !u32(all, p + 8, true, &a.offset) || !u32(all, p + 12, true, &a.size))
            return fail(error, "Fat arch table is truncated");
        if (quint64(a.offset) + a.size > quint64(all.size())) return fail(error, "Fat slice lies outside the file");
        arches.append(a);
    }
    QList<Arch> byOffset = arches;
    std::sort(byOffset.begin(), byOffset.end(), [](const Arch &x, const Arch &y) { return x.offset < y.offset; });
    for (int i = 1; i < byOffset.size(); ++i)
        if (quint64(byOffset[i - 1].offset) + byOffset[i - 1].size > byOffset[i].offset) return fail(error, "Fat slices overlap");
    for (int i = 0; i < arches.size(); ++i)
        for (int j = i + 1; j < arches.size(); ++j)
            if (arches[i].cpu == arches[j].cpu && (arches[i].sub & kSubtypeMask) == (arches[j].sub & kSubtypeMask))
                return fail(error, "Fat header has two slices for the same cpu and subtype");
    const Arch *chosen = nullptr, *other = nullptr;
    int others = 0;
    for (const Arch &a : arches) {
        if (a.cpu != kCpuArm64) continue;
        if ((a.sub & kSubtypeMask) == preferred) chosen = &a;
        else { other = &a; ++others; }
    }
    *fallback = !chosen;
    if (!chosen && allowFallback) {
        if (others > 1) return fail(error, "Fallback is ambiguous: several other arm64 slices exist");
        chosen = other;
    }
    if (!chosen) return fail(error, "Image has no slice for the requested arm64 subtype");
    *out = {all.mid(chosen->offset, chosen->size), chosen->sub & kSubtypeMask};
    return true;
}
}

namespace MachO {
bool hasMachOMagic(const QByteArray &head) {
    quint32 big;
    if (!u32(head, 0, true, &big)) return false;
    // MH_MAGIC/MH_MAGIC_64 and their byte-swapped forms, FAT_MAGIC/FAT_MAGIC_64 and theirs
    static const quint32 kMagics[] = {0xfeedface, 0xcefaedfe, 0xfeedfacf, 0xcffaedfe, 0xcafebabe, 0xbebafeca, 0xcafebabf, 0xbfbafeca};
    return std::find(std::begin(kMagics), std::end(kMagics), big) != std::end(kMagics);
}

bool arm64Subtypes(const QByteArray &head, QList<quint32> *subtypes, QString *error) {
    const QByteArrayView all(head);
    subtypes->clear();
    quint32 magic;
    if (!u32(all, 0, true, &magic)) return fail(error, "Mach-O file is too short");
    if (magic == kFatMagic) {
        quint32 n;
        if (!u32(all, 4, true, &n) || n == 0 || n > kMaxFatArch) return fail(error, "Fat header is invalid");
        for (quint32 i = 0; i < n; ++i) {
            quint32 cpu, sub;
            if (!u32(all, 8 + 20ull * i, true, &cpu) || !u32(all, 12 + 20ull * i, true, &sub)) return fail(error, "Fat arch table is truncated");
            if (cpu == kCpuArm64) subtypes->append(sub & kSubtypeMask);
        }
        return true;
    }
    quint32 cpu, sub;
    if (!u32(all, 0, false, &magic) || magic != kMhMagic64 || !u32(all, 4, false, &cpu) || !u32(all, 8, false, &sub)) return fail(error, "Not a 64-bit Mach-O image");
    if (cpu == kCpuArm64) subtypes->append(sub & kSubtypeMask);
    return true;
}

bool readDependencies(const QByteArray &image, quint32 preferredSubtype, bool allowFallback, Dependencies *out, QString *error) {
    Slice slice; bool fallback = false;
    if (!pickSlice(image, preferredSubtype, allowFallback, &slice, &fallback, error)) return false;
    Dependencies deps; quint32 sub = 0;
    if (!readThin(slice.bytes, &deps.dependencies, &sub, error)) return false;
    if (sub != slice.subtype) return fail(error, "Fat arch subtype disagrees with the slice header");
    deps.subtype = sub; deps.usedFallback = fallback;
    *out = deps;
    return true;
}

bool dependencyClosure(const QByteArray &root, const Resolver &resolve, quint32 preferredSubtype, bool allowFallback, Closure *out, QString *error) {
    struct Item { QString name; int depth; };
    Closure c; QSet<QString> seen, seenRelative; QList<Item> queue;
    auto expand = [&](const QByteArray &image, int depth, const QString &what) {
        Dependencies deps; QString err;
        if (!readDependencies(image, preferredSubtype, allowFallback, &deps, &err)) return fail(error, what + ": " + err);
        for (const MachO::Dependency &dep : deps.dependencies) {
            const QString &name = dep.installName;
            if (!name.startsWith('/')) {
                if (!seenRelative.contains(name)) {
                    if (seen.size() + seenRelative.size() >= kMaxNodes) return fail(error, "Dependency closure exceeds the node cap");
                    seenRelative.insert(name); c.unresolvableRelative.append(name); }
                continue;
            }
            if (seen.contains(name)) continue;
            if (seen.size() + seenRelative.size() >= kMaxNodes) return fail(error, "Dependency closure exceeds the node cap");
            seen.insert(name); c.installNames.append(name); queue.append({name, depth});
        }
        return true;
    };
    if (!expand(root, 1, "root image")) return false;
    for (int i = 0; i < queue.size(); ++i) {
        const Item item = queue[i];
        if (item.depth > kMaxDepth) return fail(error, "Dependency closure exceeds the depth cap at " + item.name);
        const std::optional<QByteArray> image = resolve(item.name);
        if (!image) { c.unresolved.append(item.name); continue; }
        if (!expand(*image, item.depth + 1, item.name)) return false;
    }
    *out = c;
    return true;
}
}
