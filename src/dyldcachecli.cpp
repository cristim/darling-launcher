// SPDX-License-Identifier: GPL-3.0-or-later
#include "dyldcachecli.h"
#include "dyldcacheconfig.h"
#include "dyldcachecopy.h"
#include "dyldcacheheader.h"
#include "dyldcachestore.h"
#include <QDir>
#include <QElapsedTimer>
#include <QMap>
#include <atomic>
#include <csignal>

namespace {
std::atomic<bool> gCancel{false};
extern "C" void onSigint(int) { gCancel.store(true); }

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
        else if (args[i] == "--lib") { if (i + 1 >= args.size()) return false; out->libs << args[++i]; }
        else if (valued.contains(args[i])) { if (i + 1 >= args.size()) return false; const QString key = args[i]; out->values[key] = args[++i]; }
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
int usage(QTextStream &err, const QString &why) {
    err << "usage error: " << why << "\n" << kHelp;
    return 2;
}
int refused(QTextStream &err, const QString &reason) {
    err << "error: " << reason << "\n";
    return 1;
}
// Absolute without resolving symlinks: the store code refuses those itself.
QString absolute(const QString &p) { return QDir::cleanPath(QDir(p).absolutePath()); }
bool positiveInt(const QString &s, int max, int *out) {
    bool ok = false;
    const int v = s.toInt(&ok);
    if (!ok || v < 0 || v > max) return false;
    *out = v;
    return true;
}
QString storeRoot(const Args &a) { return a.values.contains("--store-root") ? absolute(a.values["--store-root"]) : DyldCacheStore::configuredRoot(); }

qint64 totalBytes(const StoreManifest &m) { qint64 n = 0; for (const StoreFile &f : m.files) n += f.size; return n; }

int copyVerb(const Args &a, QTextStream &err) {
    const QString arch = a.values.value("--arch", "arm64e");
    int windowMib = 128, throttleMs = 0, marginMib = int(DyldCacheStore::kDefaultMarginBytes >> 20);
    if (a.values.contains("--window-mib") && (!positiveInt(a.values["--window-mib"], 4096, &windowMib) || windowMib < 1)) return usage(err, "--window-mib must be 1 to 4096");
    if (a.values.contains("--throttle-ms") && !positiveInt(a.values["--throttle-ms"], 60000, &throttleMs)) return usage(err, "--throttle-ms must be 0 to 60000");
    if (a.values.contains("--margin-mib") && !positiveInt(a.values["--margin-mib"], 1 << 20, &marginMib)) return usage(err, "--margin-mib must be 0 to 1048576");
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

    gCancel.store(false);
    struct sigaction sa{}, old{};
    sa.sa_handler = onSigint;
    sigemptyset(&sa.sa_mask);
    ::sigaction(SIGINT, &sa, &old);
    const DyldCacheCopy::Outcome r = DyldCacheCopy::copyCache(absolute(a.values["--source"]), arch, storeRoot(a), opt, progress, &gCancel);
    ::sigaction(SIGINT, &old, nullptr);

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
    if (a.values.contains("--source")) {
        if (arch != "arm64" && arch != "arm64e") return usage(err, "--arch must be arm64e or arm64");
        QByteArray head; qint64 size = 0; DyldCache::MainHeader header; QString error;
        if (!DyldCacheStore::readHead(absolute(a.values["--source"]), DyldCache::mainFileName(arch), DyldCache::kHeaderWindow, &head, &size, &error)
            || !DyldCache::parseMainHeader(head, size, &header, &error)) return refused(err, error);
        if (header.arch != arch) return refused(err, "The source cache is for " + header.arch + ", not " + arch);
        sourceId = DyldCacheStore::storeId(arch, DyldCache::uuidHex(header.uuid));
    }
    const QStringList ids = DyldCacheStore::listStores(root);
    static const QMap<StoreState, QString> names = {{StoreState::Missing, "missing"}, {StoreState::Partial, "partial"}, {StoreState::Ready, "ready"}, {StoreState::Corrupt, "corrupt"}};
    for (const QString &id : ids) {
        const StoreStatus st = DyldCacheStore::status(root, id);
        out << id << '\t' << names[st.state] << '\t' << st.manifest.files.size() << '\t' << totalBytes(st.manifest) << '\t' << st.manifest.copiedAt;
        if (!sourceId.isEmpty() && id.startsWith(arch + "-")) out << '\t' << (id == sourceId ? "current" : "stale");
        out << '\n';
    }
    if (ids.isEmpty()) out << "no store\n";
    if (!sourceId.isEmpty()) out << "source\t" << sourceId << '\t' << (ids.contains(sourceId) ? "stored" : "not stored (stale)") << '\n';
    return 0;
}

int verifyVerb(const Args &a, QTextStream &out, QTextStream &err) {
    const QString root = storeRoot(a);
    const QStringList ids = a.values.contains("--store") ? QStringList{a.values["--store"]} : DyldCacheStore::listStores(root);
    if (ids.isEmpty()) return refused(err, "There is no store to verify");
    int rc = 0;
    for (const QString &id : ids) {
        QString error;
        if (!DyldCacheStore::checkPrivate(root, id, &error)) { err << "error: " << id << ": " << error << "\n"; rc = 1; continue; }
        const DyldCacheCopy::Outcome r = DyldCacheCopy::verifyStore(root, id);
        if (r.result == DyldCacheCopy::Result::Done) out << "ok\t" << id << '\n';
        else { err << "error: " << id << ": " << r.error << "\n"; rc = 1; }
    }
    return rc;
}

int prefixVerb(const Args &a, QTextStream &out, QTextStream &err) {
    if (a.enable == a.disable) return usage(err, "prefix needs exactly one of --enable and --disable");
    if (a.disable && (!a.libs.isEmpty() || a.values.contains("--store"))) return usage(err, "--lib and --store go with --enable");
    const QString prefix = absolute(a.values["--prefix"]), root = storeRoot(a);
    QString error;
    DyldCacheConfig config;
    if (!DyldCacheConfigFile::load(prefix, &config, &error)) return refused(err, error);
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
        if (!DyldCacheStore::validStoreId(config.store)) return refused(err, "Invalid cache store id");
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
