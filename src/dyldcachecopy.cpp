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

struct Fd {
    int fd;
    explicit Fd(int f = -1) : fd(f) {}
    ~Fd() { if (fd >= 0) ::close(fd); }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
};
struct Item { QString name; qint64 size; QByteArray uuid; };
struct Tally { qint64 done = 0, total = 0; int index = 0, count = 0; };

bool cancelled(const std::atomic<bool> *c) { return c && c->load(); }
DyldCopy failed(const QString &message) { DyldCopy o; o.error = message; return o; }
bool preadFull(int fd, char *buf, qint64 len, qint64 off) {
    for (qint64 got = 0; got < len;) {
        const ssize_t n = ::pread(fd, buf + got, size_t(len - got), off + got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        got += n;
    }
    return true;
}
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
// Length of the prefix (at most `limit`) of dst that equals src, to the block; -1 on a read error, kCancelled on cancel.
// Reads the source, so it pauses between windows like the copy does.
qint64 matchingPrefix(int srcFd, int dstFd, qint64 limit, const DyldCacheCopy::Options &opt, const std::atomic<bool> *cancel, QByteArray *buf) {
    QByteArray other(buf->size(), Qt::Uninitialized);
    for (qint64 pos = 0; pos < limit;) {
        if (cancelled(cancel)) return kCancelled;
        const qint64 n = qMin<qint64>(buf->size(), limit - pos);
        if (!preadFull(srcFd, buf->data(), n, pos) || !preadFull(dstFd, other.data(), n, pos)) return -1;
        if (std::memcmp(buf->constData(), other.constData(), size_t(n)) != 0) return pos;
        pos += n;
        if (opt.pauseMs > 0 && pos % opt.windowBytes == 0 && pos < limit) { if (opt.sleepFn) opt.sleepFn(opt.pauseMs); else QThread::msleep(unsigned(opt.pauseMs)); }
    }
    return limit;
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
        const qint64 kept = matchingPrefix(src.fd, dst.fd, existing / opt.windowBytes * opt.windowBytes, opt, cancel, &buf);
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
        if (opt.pauseMs > 0 && pos < srcSize) { if (opt.sleepFn) opt.sleepFn(opt.pauseMs); else QThread::msleep(unsigned(opt.pauseMs)); }
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
    if (before.state == StoreState::Ready || before.state == StoreState::Stale) { Outcome o; o.result = Result::Done; o.storeId = id; return o; }
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
        if (finalDone[i]) { tally.done += items[i].size; continue; }
        const Outcome r = copyFile(sourceDir, storeRoot, id, arch, items[i], opt, progress, cancel, &tally);
        if (r.result != Result::Done) { Outcome o = r; o.storeId = id; return o; }
    }

    StoreManifest m; m.arch = arch; m.uuid = uuidHex(header.uuid); m.buildLabel = opt.buildLabel;
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
