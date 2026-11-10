// SPDX-License-Identifier: GPL-3.0-or-later
#include "dyldcachecli.h"
#include "dyldcacheconfig.h"
#include "dyldcachecopy.h"
#include "dyldcacheheader.h"
#include "dyldcachestore.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMap>
#include <atomic>
#include <csignal>

namespace {
std::atomic<bool> gCancel{false};
// The first Ctrl-C asks the copy to stop; it then restores the default action, so a second one kills the process at once.
extern "C" void onSigint(int) {
    gCancel.store(true);
    struct sigaction dfl{};
    dfl.sa_handler = SIG_DFL;
    ::sigaction(SIGINT, &dfl, nullptr);
}
// For the length of a copy: SIGINT is a clean cancel, and SIGXFSZ (a file size limit) is ignored so that a write over
// the limit fails with an error instead of killing the process. Restores the previous handlers.
struct CopySignals {
    struct sigaction oldInt{}, oldXfsz{};
    CopySignals() {
        gCancel.store(false);
        struct sigaction cancel{}, ignore{};
        cancel.sa_handler = onSigint;
        sigemptyset(&cancel.sa_mask);
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        ::sigaction(SIGINT, &cancel, &oldInt);
        ::sigaction(SIGXFSZ, &ignore, &oldXfsz);
    }
    ~CopySignals() {
        ::sigaction(SIGINT, &oldInt, nullptr);
        ::sigaction(SIGXFSZ, &oldXfsz, nullptr);
    }
};

struct Args {
    QMap<QString, QString> values;
    QStringList libs;
    bool enable = false, disable = false;
};
bool parse(const QStringList &args, Args *out) {
    static const QStringList valued = {"--source", "--store-root", "--arch", "--window-mib", "--throttle-ms", "--margin-mib", "--prefix", "--store"};
    for (int i = 0; i < args.size(); ++i) {
        if (args[i] == "--enable") out->enable = true;
        else if (args[i] == "--disable") out->disable = true;
        else if (args[i] == "--lib") { if (i + 1 >= args.size() || args[i + 1].isEmpty()) return false; out->libs << args[++i]; }
        else if (valued.contains(args[i])) {
            // an empty value would resolve to the current directory, and a repeated option would silently win or lose
            if (i + 1 >= args.size() || args[i + 1].isEmpty() || out->values.contains(args[i])) return false;
            const QString key = args[i]; out->values[key] = args[++i];
        }
        else return false;
    }
    return true;
}
const char *const kHelp =
    "usage: darling-launcher dyld-cache <verb> [options]\n"
    "  copy   --source DIR [--store-root R] [--arch arm64e|arm64] [--window-mib N] [--throttle-ms N] [--margin-mib N]\n"
    "  status [--source DIR] [--store-root R] [--arch arm64e|arm64]\n"
    "  verify [--store-root R] [--store ID]\n"
    "  prefix --prefix P (--enable [--store ID] [--lib INSTALLNAME]... | --disable) [--store-root R]\n"
    "exit codes: 0 ok, 1 refused or failed, 2 usage, 130 copy cancelled (partial copy kept, run again to resume)\n";
// Text that comes from the command line or from files in the store must not break the one-line shape of the output.
QString oneLine(const QString &text) {
    QString out;
    for (const QChar c : text) out += (c.unicode() < 0x20 || c.unicode() == 0x7f) ? QString("\\x%1").arg(int(c.unicode()), 2, 16, QChar(u'0')) : QString(c);
    return out;
}
int usage(QTextStream &err, const QString &why) {
    err << "usage error: " << oneLine(why) << "\n" << kHelp;
    return 2;
}
int refused(QTextStream &err, const QString &reason) {
    err << "error: " << oneLine(reason) << "\n";
    return 1;
}
// Absolute without resolving symlinks: the store code refuses those itself.
QString absolute(const QString &p) { return QDir::cleanPath(QDir(p).absolutePath()); }
// Absent option: *out keeps its default. Present: a whole number in [min, max].
bool intOption(const Args &a, const char *key, int min, int max, int *out) {
    if (!a.values.contains(key)) return true;
    bool ok = false;
    const int v = a.values[key].toInt(&ok);
    if (!ok || v < min || v > max) return false;
    *out = v;
    return true;
}
bool validArch(const Args &a) { const QString arch = a.values.value("--arch", "arm64e"); return arch == "arm64e" || arch == "arm64"; }
QString storeRoot(const Args &a) { return a.values.contains("--store-root") ? absolute(a.values["--store-root"]) : DyldCacheStore::configuredRoot(); }

qint64 totalBytes(const StoreManifest &m) { qint64 n = 0; for (const StoreFile &f : m.files) n += f.size; return n; }

int copyVerb(const Args &a, QTextStream &err) {
    const QString arch = a.values.value("--arch", "arm64e");
    int windowMib = 128, throttleMs = 0, marginMib = int(DyldCacheStore::kDefaultMarginBytes >> 20);
    if (!validArch(a)) return usage(err, "--arch must be arm64e or arm64");
    if (!intOption(a, "--window-mib", 1, 4096, &windowMib)) return usage(err, "--window-mib must be 1 to 4096");
    if (!intOption(a, "--throttle-ms", 0, 60000, &throttleMs)) return usage(err, "--throttle-ms must be 0 to 60000");
    if (!intOption(a, "--margin-mib", 0, 1 << 20, &marginMib)) return usage(err, "--margin-mib must be 0 to 1048576");
    DyldCacheCopy::Options opt;
    opt.windowBytes = qint64(windowMib) << 20;
    opt.pauseMs = throttleMs;
    opt.marginBytes = qint64(marginMib) << 20;

    // A line at the start of each file and then at most one per second, so a 6 GB copy stays readable.
    QElapsedTimer since; since.start();
    QString lastFile;
    const auto progress = [&](const DyldCacheCopy::Progress &p) {
        if (p.fileName == lastFile && since.elapsed() < 1000) return;
        lastFile = p.fileName; since.restart();
        err << "[" << p.fileIndex << "/" << p.fileCount << "] " << p.fileName << " " << (p.fileBytes >> 20) << "/" << (p.fileSize >> 20)
            << " MiB, total " << (p.totalBytes >> 20) << "/" << (p.totalSize >> 20) << " MiB\n";
        err.flush();
    };

    const QString root = storeRoot(a);
    // A mistyped location must not make this create directories; only the launcher's own default location gets its parents made.
    if (a.values.contains("--store-root") && !QFileInfo(QFileInfo(root).absolutePath()).isDir()) return refused(err, "The parent directory of --store-root does not exist");
    DyldCacheCopy::Outcome r;
    { const CopySignals handlers; r = DyldCacheCopy::copyCache(absolute(a.values["--source"]), arch, root, opt, progress, &gCancel); }

    if (r.result == DyldCacheCopy::Result::Cancelled) {
        err << "cancelled: the partial copy is kept, run the same command again to resume\n";
        return 130;
    }
    if (r.result == DyldCacheCopy::Result::Failed) return refused(err, r.error);
    err << "done: store " << r.storeId << "\n";
    return 0;
}

int statusVerb(const Args &a, QTextStream &out, QTextStream &err) {
    const QString root = storeRoot(a), arch = a.values.value("--arch", "arm64e");
    QString sourceId;
    if (!validArch(a)) return usage(err, "--arch must be arm64e or arm64");
    if (a.values.contains("--source")) {
        QByteArray head; qint64 size = 0; DyldCache::MainHeader header; QString error;
        if (!DyldCacheStore::readHead(absolute(a.values["--source"]), DyldCache::mainFileName(arch), DyldCache::kHeaderWindow, &head, &size, &error)
            || !DyldCache::parseMainHeader(head, size, &header, &error)) return refused(err, error);
        if (header.arch != arch) return refused(err, "The source cache is for " + header.arch + ", not " + arch);
        sourceId = DyldCacheStore::storeId(arch, DyldCache::uuidHex(header.uuid));
    }
    const QFileInfo rootInfo(root);
    if (rootInfo.exists() && (!rootInfo.isDir() || !QDir(root).isReadable())) return refused(err, "The store location is not a readable directory");
    const QStringList ids = DyldCacheStore::listStores(root);
    QStringList ready;
    static const QMap<StoreState, QString> names = {{StoreState::Missing, "missing"}, {StoreState::Partial, "partial"}, {StoreState::Ready, "ready"}, {StoreState::Corrupt, "corrupt"}};
    for (const QString &id : ids) {
        const StoreStatus st = DyldCacheStore::status(root, id);
        if (st.state == StoreState::Ready) ready << id;
        out << id << '\t' << names[st.state] << '\t' << st.manifest.files.size() << '\t' << totalBytes(st.manifest) << '\t' << oneLine(st.manifest.copiedAt);
        // only a complete store can be current or stale
        if (!sourceId.isEmpty() && id.startsWith(arch + "-")) out << '\t' << (st.state != StoreState::Ready ? "-" : id == sourceId ? "current" : "stale");
        out << '\n';
    }
    if (ids.isEmpty()) out << "no store\n";
    if (!sourceId.isEmpty()) out << "source\t" << sourceId << '\t' << (ready.contains(sourceId) ? "stored" : ids.contains(sourceId) ? "store not ready" : "not stored (stale)") << '\n';
    return 0;
}

int verifyVerb(const Args &a, QTextStream &out, QTextStream &err) {
    const QString root = storeRoot(a);
    const QStringList ids = a.values.contains("--store") ? QStringList{a.values["--store"]} : DyldCacheStore::listStores(root);
    if (ids.isEmpty()) return refused(err, "There is no store to verify");
    int rc = 0;
    for (const QString &id : ids) {
        QString error;
        if (!DyldCacheStore::checkPrivate(root, id, &error)) { refused(err, id + ": " + error); rc = 1; continue; }
        const DyldCacheCopy::Outcome r = DyldCacheCopy::verifyStore(root, id);
        if (r.result == DyldCacheCopy::Result::Done) out << "ok\t" << id << '\n';
        else { refused(err, id + ": " + r.error); rc = 1; }
    }
    return rc;
}

int prefixVerb(const Args &a, QTextStream &out, QTextStream &err) {
    if (a.enable == a.disable) return usage(err, "prefix needs exactly one of --enable and --disable");
    if (a.disable && (!a.libs.isEmpty() || a.values.contains("--store"))) return usage(err, "--lib and --store go with --enable");
    const QString prefix = absolute(a.values["--prefix"]), root = storeRoot(a);
    // The prefix config records a store id only; a launch looks for it in the location from the launcher settings.
    if (root != absolute(DyldCacheStore::configuredRoot())) return refused(err, "--store-root is not the store location in the launcher settings, so a launch would not find this store");
    QString error;
    DyldCacheConfig config;
    // load() leaves the default (off) behind when the file is unreadable, and "off" must still replace such a file,
    // or the prefix could not be switched back.
    if (!DyldCacheConfigFile::load(prefix, &config, &error) && a.enable) return refused(err, error);
    if (a.disable) {
        config.mode = DyldCacheMode::Off;
    } else {
        config.mode = DyldCacheMode::AllowList;
        if (a.values.contains("--store")) config.store = a.values["--store"];
        if (config.store.isEmpty()) {
            QStringList ready;
            for (const QString &id : DyldCacheStore::listStores(root)) if (DyldCacheStore::status(root, id).state == StoreState::Ready) ready << id;
            if (ready.size() != 1) return refused(err, "Choose the store with --store: " + QString::number(ready.size()) + " ready stores found");
            config.store = ready.first();
        }
        if (!DyldCacheStore::checkPrivate(root, config.store, &error)) return refused(err, error); // as the launch path does
        if (DyldCacheStore::status(root, config.store).state != StoreState::Ready) return refused(err, "The store " + config.store + " is not ready; copy and verify it first");
        if (!a.libs.isEmpty()) config.served = a.libs;
        else if (config.served.isEmpty()) config.served = DyldCacheConfigFile::validatedLibraries();
    }
    if (!DyldCacheConfigFile::save(prefix, config, &error)) return refused(err, error);
    out << (a.enable ? "ok enabled\n" : "ok disabled\n");
    return 0;
}
}

int dyldCacheMain(const QStringList &args, QTextStream &out, QTextStream &err) {
    if (args.isEmpty()) return usage(err, "missing verb");
    const QString verb = args[0];
    if (verb == "--help" || verb == "-h" || verb == "help") { out << kHelp; return 0; }
    Args a;
    if (!parse(args.mid(1), &a)) return usage(err, "bad or incomplete option");
    const bool known = verb == "copy" || verb == "status" || verb == "verify" || verb == "prefix";
    if (!known) return usage(err, "unknown verb " + verb);
    const auto only = [&](std::initializer_list<const char *> allowed) {
        for (auto it = a.values.constBegin(); it != a.values.constEnd(); ++it) {
            bool ok = false;
            for (const char *k : allowed) ok = ok || it.key() == k;
            if (!ok) return false;
        }
        return true;
    };
    if (verb == "copy") {
        if (!a.libs.isEmpty() || a.enable || a.disable || !only({"--source", "--store-root", "--arch", "--window-mib", "--throttle-ms", "--margin-mib"})) return usage(err, "option not valid for copy");
        if (!a.values.contains("--source")) return usage(err, "copy needs --source");
        return copyVerb(a, err);
    }
    if (verb == "status") {
        if (!a.libs.isEmpty() || a.enable || a.disable || !only({"--source", "--store-root", "--arch"})) return usage(err, "option not valid for status");
        return statusVerb(a, out, err);
    }
    if (verb == "verify") {
        if (!a.libs.isEmpty() || a.enable || a.disable || !only({"--store-root", "--store"})) return usage(err, "option not valid for verify");
        return verifyVerb(a, out, err);
    }
    if (!only({"--prefix", "--store-root", "--store"})) return usage(err, "option not valid for prefix");
    if (!a.values.contains("--prefix")) return usage(err, "--prefix is required");
    return prefixVerb(a, out, err);
}
