// SPDX-License-Identifier: GPL-3.0-or-later
#include "libscli.h"
#include "imagesource.h"
#include "libraryops.h"
#include "librarystatus.h"
#include <QFile>
#include <QMap>
#include <QTemporaryDir>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
const qint64 kMaxImageBytes = qint64(512) << 20;
struct Args {
    QMap<QString, QString> values;
    QStringList positional;
    bool force = false;
};
bool parse(const QStringList &args, Args *out) {
    static const QStringList valued = {"--prefix", "--runtime-root", "--name", "--file", "--source-kind"};
    for (int i = 0; i < args.size(); ++i) {
        if (args[i] == "--force") out->force = true;
        else if (valued.contains(args[i])) { if (i + 1 >= args.size()) return false; const QString key = args[i]; out->values[key] = args[++i]; }
        else if (args[i].startsWith("--")) return false;
        else out->positional << args[i];
    }
    return true;
}
const char *const kHelp =
    "usage: darling-launcher libs <verb> [options]\n"
    "  list    --prefix P [--runtime-root R]\n"
    "  import  --prefix P --runtime-root R --name INSTALLNAME --file PATH [--force] [--source-kind volume|fixture]\n"
    "  enable  --prefix P --runtime-root R --name INSTALLNAME [--force]\n"
    "  disable --prefix P --name INSTALLNAME\n"
    "  abandon --prefix P --name INSTALLNAME\n"
    "  master  on|off --prefix P [--runtime-root R] [--force]   (on needs --runtime-root)\n"
    "exit codes: 0 ok, 1 refused or failed, 2 usage, 3 master off left files alone because this launcher did not write them\n";
int usage(QTextStream &err, const QString &why) {
    err << "usage error: " << why << "\n" << kHelp;
    return 2;
}
// Opens without blocking or following a symlink, then checks the descriptor itself (no check-then-open gap).
bool readRegularFile(const QString &path, QByteArray *bytes, QString *error) {
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { *error = "cannot open " + path + ": " + QString::fromLocal8Bit(strerror(errno)); return false; }
    struct stat st;
    bool ok = false;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) *error = "not a regular file: " + path;
    else if (st.st_size > kMaxImageBytes) *error = "file is larger than 512 MiB: " + path;
    else {
        bytes->resize(qsizetype(st.st_size));
        ok = true;
        for (qint64 off = 0; ok && off < bytes->size();) {
            const ssize_t n = ::read(fd, bytes->data() + off, size_t(bytes->size() - off));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { *error = "cannot read " + path; ok = false; } else off += n;
        }
    }
    ::close(fd);
    return ok;
}
int refused(QTextStream &err, const QString &reason) {
    err << "error: " << reason << "\n";
    return 1;
}
}

int libsMain(const QStringList &args, QTextStream &out, QTextStream &err) {
    if (args.isEmpty()) return usage(err, "missing verb");
    const QString verb = args[0];
    if (verb == "--help" || verb == "-h" || verb == "help") { out << kHelp; return 0; }
    Args a;
    if (!parse(args.mid(1), &a)) return usage(err, "bad or incomplete option");
    const QString prefix = a.values.value("--prefix"), runtime = a.values.value("--runtime-root"), name = a.values.value("--name");
    if (prefix.isEmpty()) return usage(err, "--prefix is required");
    const auto need = [&](bool ok, const char *what) { return ok ? 0 : usage(err, what); };
    ImportOptions options;
    QString error;
    if (verb == "list") {
        if (!a.positional.isEmpty()) return usage(err, "unexpected argument");
        LibrariesFile file;
        if (!Libraries::load(prefix, &file, &error)) return refused(err, error);
        QTemporaryDir empty; // without --runtime-root nothing is reported as builtin
        for (const LibraryRecord &r : file.libraries) {
            LibraryStatus status;
            if (!libraryStatus(prefix, runtime.isEmpty() ? empty.path() : runtime, r.installName, &status, &error)) return refused(err, error);
            out << r.installName << '\t' << libraryStatusName(status) << '\t' << r.source.kind << '\n';
        }
        return 0;
    }
    if (verb == "import") {
        if (!a.positional.isEmpty()) return usage(err, "unexpected argument");
        if (const int rc = need(!runtime.isEmpty() && !name.isEmpty() && a.values.contains("--file"), "import needs --runtime-root, --name and --file")) return rc;
        const QString kind = a.values.value("--source-kind", "volume");
        if (kind != "volume" && kind != "fixture") return usage(err, "--source-kind must be volume or fixture");
        QByteArray bytes;
        if (!readRegularFile(a.values.value("--file"), &bytes, &error)) return refused(err, error);
        const LibrarySource source{kind, "", "libs-cli", ImageSources::sha256Hex(bytes)};
        if (!importImage(prefix, runtime, name, bytes, source, a.force, &error, options)) return refused(err, error);
    } else if (verb == "enable") {
        if (const int rc = need(!runtime.isEmpty() && !name.isEmpty(), "enable needs --runtime-root and --name")) return rc;
        if (!enableLibrary(prefix, runtime, name, a.force, &error, options)) return refused(err, error);
    } else if (verb == "disable" || verb == "abandon") {
        if (const int rc = need(!name.isEmpty(), "--name is required")) return rc;
        if (!(verb == "disable" ? disableLibrary(prefix, name, &error, options) : abandonLibrary(prefix, name, &error, options))) return refused(err, error);
    } else if (verb == "master") {
        if (a.positional.size() != 1 || (a.positional[0] != "on" && a.positional[0] != "off")) return usage(err, "master needs on or off");
        const bool on = a.positional[0] == "on";
        if (const int rc = need(!on || !runtime.isEmpty(), "master on needs --runtime-root")) return rc;
        QStringList skipped;
        if (!setMasterSwitch(prefix, runtime, on, a.force, &skipped, &error, options)) return refused(err, error);
        if (!skipped.isEmpty()) {
            err << "warning: left alone, not written by this launcher: " << skipped.join(", ") << "\n";
            return 3;
        }
    } else {
        return usage(err, "unknown verb " + verb);
    }
    out << "ok\n";
    return 0;
}
