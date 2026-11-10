// SPDX-License-Identifier: GPL-3.0-or-later
#include "dyldcachecopy.h"
#include "core.h"
#include "safefs.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QThread>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using LauncherCore::fail;
using namespace DyldCache;
using namespace DyldCacheStore;
using DyldCopy = DyldCacheCopy::Outcome;
constexpr int kHeaderProbe = 0x58 + kUuidBytes;

using SafeFs::Fd;
using SafeFs::preadFull;
struct Item { QString name; qint64 size; QByteArray uuid; };
struct Tally {
    qint64 done = 0, total = 0;
    int index = 0, count = 0;
    bool windowRead = false; // a window of source bytes was read earlier, in this file or an earlier one
};

bool cancelled(const std::atomic<bool> *c) { return c && c->load(); }
DyldCopy failed(const QString &message) { DyldCopy o; o.error = message; return o; }
bool pwriteFull(int fd, const char *buf, qint64 len, qint64 off) {
    for (qint64 put = 0; put < len;) {
        const ssize_t n = ::pwrite(fd, buf + put, size_t(len - put), off + put);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        put += n;
    }
    return true;
}
bool headerMatches(const QByteArray &head, const QString &arch, const QByteArray &uuid) {
    QString a; QByteArray u; QString ignored;
    return parseFileHeader(head, &a, &u, &ignored) && a == arch && u == uuid;
}

constexpr qint64 kCancelled = -2;
constexpr int kPauseSliceMs = 100;
// Waits pauseMs in short slices so a cancel is seen within one slice. False when cancelled.
bool pauseFor(const DyldCacheCopy::Options &opt, const std::atomic<bool> *cancel) {
    for (int left = opt.pauseMs; left > 0 && !cancelled(cancel);) {
        const int slice = qMin(left, kPauseSliceMs);
        if (opt.sleepFn) opt.sleepFn(slice); else QThread::msleep(unsigned(slice));
        left -= slice;
    }
    return !cancelled(cancel);
}
// Called before every window of source reads, whether it checks a kept prefix or copies: the pause falls between any two
// windows, across file boundaries and between the check and the copy, so the source never sees back-to-back windows.
bool beforeWindow(const DyldCacheCopy::Options &opt, const std::atomic<bool> *cancel, Tally *tally) {
    const bool pause = tally->windowRead && opt.pauseMs > 0;
    tally->windowRead = true;
    return !pause || pauseFor(opt, cancel);
}
// Length of the prefix (at most `limit`) of dst that equals src, to the block; -1 on a read error, kCancelled on cancel.
// Reads the source, so it pauses after every window's worth of bytes like the copy does. onBlock gets the verified length.
qint64 matchingPrefix(int srcFd, int dstFd, qint64 limit, const DyldCacheCopy::Options &opt, const std::atomic<bool> *cancel, QByteArray *buf, Tally *tally,
                      const std::function<void(qint64)> &onBlock = {}) {
    QByteArray other(buf->size(), Qt::Uninitialized);
    qint64 sinceWindow = 0;
    for (qint64 pos = 0; pos < limit;) {
        if (cancelled(cancel)) return kCancelled;
        if (sinceWindow == 0 && !beforeWindow(opt, cancel, tally)) return kCancelled;
        const qint64 n = qMin<qint64>(buf->size(), limit - pos);
        if (!preadFull(srcFd, buf->data(), n, pos) || !preadFull(dstFd, other.data(), n, pos)) return -1;
        if (std::memcmp(buf->constData(), other.constData(), size_t(n)) != 0) return pos;
        pos += n; sinceWindow += n;
        if (onBlock) onBlock(pos);
        if (sinceWindow >= opt.windowBytes) sinceWindow = 0;
    }
    return limit;
}

// A store file that exists before the manifest does (a copy cut short after renaming it) is trusted only after it is
// compared with the source: another process of this user could have planted a file with a valid header.
DyldCopy checkDoneFile(const QString &sourceDir, const QString &root, const QString &id, const Item &item, const DyldCacheCopy::Options &opt,
                       const DyldCacheCopy::ProgressFn &progress, const std::atomic<bool> *cancel, Tally *tally) {
    QString err;
    Fd src(SafeFs::openRead(sourceDir, item.name, nullptr, &err));
    if (src.fd < 0) return failed(err);
    Fd dst(SafeFs::openRead(root, id + "/" + item.name, nullptr, &err)); // its size was checked when it was found
    if (dst.fd < 0) return failed(err);
    QByteArray buf(int(qMin(opt.blockBytes, item.size ? item.size : qint64(1))), Qt::Uninitialized);
    const qint64 base = tally->done;
    const qint64 same = matchingPrefix(src.fd, dst.fd, item.size, opt, cancel, &buf, tally, [&](qint64 pos) {
        if (progress) progress({tally->index, tally->count, item.name, pos, item.size, base + pos, tally->total});
    });
    if (same == kCancelled) { DyldCopy c; c.result = DyldCacheCopy::Result::Cancelled; return c; }
    if (same < 0) return failed("Cannot compare " + item.name + " in the store with the source");
    if (same != item.size) return failed(item.name + " in the store differs from the source. Move the store directory aside and copy again.");
    tally->done += item.size;
    DyldCopy ok; ok.result = DyldCacheCopy::Result::Done;
    return ok;
}

// Copies one file into <id>/<name>.partial and renames it into place. Returns Done, Cancelled or Failed.
DyldCopy copyFile(const QString &sourceDir, const QString &root, const QString &id, const QString &arch, const Item &item,
                  const DyldCacheCopy::Options &opt, const DyldCacheCopy::ProgressFn &progress, const std::atomic<bool> *cancel, Tally *tally) {
    QString err;
    qint64 srcSize = 0;
    Fd src(SafeFs::openRead(sourceDir, item.name, &srcSize, &err));
    if (src.fd < 0) return failed(err);
    if (srcSize != item.size) return failed(item.name + " changed on the source while copying");
    const QString partial = id + "/" + item.name + ".partial";
    qint64 existing = 0;
    Fd dst(SafeFs::openWrite(root, partial, 0600, 0700, true, &existing, &err));
    if (dst.fd < 0) return failed(err);
    QByteArray buf(int(qMin(opt.blockBytes, srcSize ? srcSize : qint64(1))), Qt::Uninitialized);
    // Only whole windows were flushed, and anything in the partial could have been changed by another process of this
    // user, so the kept prefix is proven equal to the source and cut at the first block that differs.
    qint64 offset = 0;
    if (existing <= srcSize) {
        const qint64 kept = matchingPrefix(src.fd, dst.fd, existing / opt.windowBytes * opt.windowBytes, opt, cancel, &buf, tally, [&](qint64 pos) {
            if (progress) progress({tally->index, tally->count, item.name, pos, srcSize, tally->done + pos, tally->total});
        });
        if (kept == kCancelled) { DyldCopy c; c.result = DyldCacheCopy::Result::Cancelled; return c; }
        if (kept < 0) return failed("Cannot compare the partial copy of " + item.name + " with the source");
        offset = kept;
    }
    if (::ftruncate(dst.fd, offset) != 0) return failed("Cannot prepare " + item.name + " for writing");
    tally->done += offset;
    DyldCopy ok; ok.result = DyldCacheCopy::Result::Done;
    auto report = [&](qint64 pos) {
        if (progress) progress({tally->index, tally->count, item.name, pos, srcSize, tally->done, tally->total});
    };
    report(offset);
    for (qint64 pos = offset; pos < srcSize;) {
        if (!beforeWindow(opt, cancel, tally)) { DyldCopy c; c.result = DyldCacheCopy::Result::Cancelled; return c; }
        const qint64 windowEnd = qMin(pos + opt.windowBytes, srcSize);
        if (opt.onWindow) opt.onWindow(item.name, pos, windowEnd - pos);
        while (pos < windowEnd) {
            if (cancelled(cancel)) { DyldCopy c; c.result = DyldCacheCopy::Result::Cancelled; return c; }
            const qint64 n = qMin<qint64>(buf.size(), windowEnd - pos);
            if (!preadFull(src.fd, buf.data(), n, pos)) return failed("Cannot read " + item.name + " from the source (truncated or unreadable)");
            if (!pwriteFull(dst.fd, buf.constData(), n, pos)) return failed("Cannot write " + item.name + " to the store");
            pos += n; tally->done += n;
            report(pos);
        }
        if (::fdatasync(dst.fd) != 0) return failed("Cannot flush " + item.name + " to the store");
        ::posix_fadvise(src.fd, 0, 0, POSIX_FADV_DONTNEED);
    }
    struct stat st, ss;
    if (::fstat(dst.fd, &st) != 0 || st.st_size != srcSize) return failed(item.name + " has the wrong size after copying");
    if (::fstat(src.fd, &ss) != 0 || ss.st_size != srcSize) return failed(item.name + " changed on the source while copying");
    QByteArray head(kHeaderProbe, Qt::Uninitialized);
    if (srcSize < kHeaderProbe || !preadFull(dst.fd, head.data(), head.size(), 0) || !headerMatches(head, arch, item.uuid))
        return failed(item.name + " does not carry the expected cache UUID after copying");
    if (::fchmod(dst.fd, 0400) != 0) return failed("Cannot protect " + item.name);
    if (!SafeFs::renameInDir(root, id, item.name + ".partial", item.name, &err, true)) return failed(err);
    return ok;
}

bool sha256Of(const QString &root, const QString &rel, QString *hex, const std::atomic<bool> *cancel, QString *error,
              const std::function<void(qint64)> &onBytes) {
    qint64 size = 0;
    Fd fd(SafeFs::openRead(root, rel, &size, error));
    if (fd.fd < 0) return false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buf(1 << 20, Qt::Uninitialized);
    for (qint64 pos = 0; pos < size;) {
        if (cancelled(cancel)) return fail(error, QString());
        const qint64 n = qMin<qint64>(buf.size(), size - pos);
        if (!preadFull(fd.fd, buf.data(), n, pos)) return fail(error, "Cannot read " + rel.section('/', -1));
        hash.addData(QByteArrayView(buf.constData(), n)); pos += n;
        if (onBytes) onBytes(n);
    }
    *hex = QString::fromLatin1(hash.result().toHex());
    return true;
}
}

namespace DyldCacheCopy {
Outcome copyCache(const QString &sourceDir, const QString &arch, const QString &storeRoot, const Options &opt,
                  const ProgressFn &progress, const std::atomic<bool> *cancel) {
    QString err;
    if (arch != "arm64" && arch != "arm64e") return failed("Unsupported cache architecture");
    if (!sourceDir.startsWith('/')) return failed("The source must be an absolute directory");
    if (opt.windowBytes <= 0 || opt.blockBytes <= 0) return failed("Window and block sizes must be positive");
    if (opt.blockBytes > DyldCacheCopy::kMaxBlockBytes) return failed("The read block size is too large");

    QByteArray head; qint64 mainSize = 0; MainHeader header;
    if (!readHead(sourceDir, mainFileName(arch), kHeaderWindow, &head, &mainSize, &err)) return failed(err);
    if (!parseMainHeader(head, mainSize, &header, &err)) return failed(err);
    if (header.arch != arch) return failed("The source cache is for " + header.arch + ", not " + arch);

    QList<Item> items{{mainFileName(arch), mainSize, header.uuid}};
    for (const SubCache &s : header.subCaches) {
        const QString name = mainFileName(arch) + s.suffix;
        QByteArray subHead; qint64 size = 0; QString subArch; QByteArray subUuid;
        if (!readHead(sourceDir, name, kHeaderProbe, &subHead, &size, &err)) return failed(err);
        if (!parseFileHeader(subHead, &subArch, &subUuid, &err)) return failed(name + ": " + err);
        if (subArch != arch || subUuid != s.uuid) return failed(name + " does not belong to the main cache file");
        items.append({name, size, s.uuid});
    }
    const QString id = storeId(arch, uuidHex(header.uuid));

    if (!ensureRoot(storeRoot, &err)) return failed(err);
    Fd lock(SafeFs::lockFile(storeRoot, ".copy.lock", 0700, &err));
    if (lock.fd < 0) return failed(err);

    const auto before = status(storeRoot, id);
    if (before.state == StoreState::Ready) { Outcome o; o.result = Result::Done; o.storeId = id; return o; }
    if (before.state == StoreState::Corrupt) return failed("The existing store for this cache build is damaged: " + before.detail + ". Move it aside and copy again.");

    Tally tally; tally.count = int(items.size());
    qint64 needed = 0;
    QList<bool> finalDone;
    for (const Item &it : items) {
        tally.total += it.size;
        QByteArray fh; qint64 fsize = 0; QString e;
        const bool done = readHead(storeRoot, id + "/" + it.name, kHeaderProbe, &fh, &fsize, &e) && fsize == it.size && headerMatches(fh, arch, it.uuid);
        finalDone.append(done);
        if (done) continue;
        QByteArray ph; qint64 psize = 0;
        needed += it.size - (readHead(storeRoot, id + "/" + it.name + ".partial", 0, &ph, &psize, &e) ? qMin(psize, it.size) : 0);
    }
    if (!checkFreeSpace(storeRoot, needed, opt.marginBytes, &err, opt.freeBytes ? opt.freeBytes : FreeBytesFn(&DyldCacheStore::freeBytes))) return failed(err);

    for (int i = 0; i < items.size(); ++i) {
        tally.index = i + 1;
        if (finalDone[i]) {
            const Outcome r = checkDoneFile(sourceDir, storeRoot, id, items[i], opt, progress, cancel, &tally);
            if (r.result != Result::Done) { Outcome o = r; o.storeId = id; return o; }
            continue;
        }
        const Outcome r = copyFile(sourceDir, storeRoot, id, arch, items[i], opt, progress, cancel, &tally);
        if (r.result != Result::Done) { Outcome o = r; o.storeId = id; return o; }
    }

    StoreManifest m; m.arch = arch; m.uuid = uuidHex(header.uuid);
    m.copiedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    for (const Item &it : items) {
        StoreFile f{it.name, it.size, uuidHex(it.uuid), QString()};
        if (!sha256Of(storeRoot, id + "/" + it.name, &f.sha256, cancel, &err, nullptr)) {
            if (cancelled(cancel)) { Outcome o; o.result = Result::Cancelled; o.storeId = id; return o; }
            return failed(err);
        }
        m.files.append(f);
    }
    if (!saveManifest(storeRoot, id, m, &err)) return failed(err);
    const auto after = status(storeRoot, id);
    if (after.state != StoreState::Ready) return failed("The copied store did not verify: " + after.detail);
    Outcome o; o.result = Result::Done; o.storeId = id;
    return o;
}

Outcome verifyStore(const QString &storeRoot, const QString &id, const ProgressFn &progress, const std::atomic<bool> *cancel) {
    const auto st = status(storeRoot, id);
    if (st.state == StoreState::Missing || st.state == StoreState::Partial) return failed("The store is not complete");
    if (st.state == StoreState::Corrupt) return failed(st.detail);
    Tally tally; tally.count = int(st.manifest.files.size());
    for (const auto &f : st.manifest.files) tally.total += f.size;
    QString err;
    for (const auto &f : st.manifest.files) {
        ++tally.index;
        QString hex; const qint64 base = tally.done;
        const bool ok = sha256Of(storeRoot, id + "/" + f.name, &hex, cancel, &err, [&](qint64 n) {
            tally.done += n;
            if (progress) progress({tally.index, tally.count, f.name, tally.done - base, f.size, tally.done, tally.total});
        });
        if (!ok) { if (cancelled(cancel)) { Outcome o; o.result = Result::Cancelled; return o; } return failed(err); }
        if (hex != f.sha256) return failed(f.name + " differs from the recorded checksum");
    }
    Outcome o; o.result = Result::Done; o.storeId = id;
    return o;
}
}
