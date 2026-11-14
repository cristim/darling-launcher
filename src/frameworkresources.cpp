// SPDX-License-Identifier: GPL-3.0-or-later
#include "frameworkresources.h"
#include "core.h"
#include "macho.h"
#include "safefs.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QStorageInfo>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace FrameworkResources {
namespace {
using LauncherCore::fail;
using LauncherCore::within;

constexpr int kMaxDepth = 32;
constexpr int kMaxFrameworkNesting = 4;
const char *const kRoots[] = {"System/Library/Frameworks", "System/Library/PrivateFrameworks"};
const QStringList kCodeDirs{"XPCServices", "PlugIns", "Helpers", "MacOS"};
const QStringList kCodeDirSuffixes{".app", ".xpc", ".appex", ".kext", ".framework"};
const QStringList kCodeFileSuffixes{".dylib", ".so", ".a", ".o"};

bool hasSuffix(const QString &name, const QStringList &suffixes) {
    return std::any_of(suffixes.begin(), suffixes.end(), [&](const QString &s) { return name.endsWith(s, Qt::CaseInsensitive); });
}
bool isCodeDir(const QString &name) { return kCodeDirs.contains(name) || hasSuffix(name, kCodeDirSuffixes); }

QByteArray readHead(int fd) {
    char buf[4]; ssize_t n;
    do n = ::pread(fd, buf, sizeof(buf), 0); while (n < 0 && errno == EINTR);
    return n > 0 ? QByteArray(buf, int(n)) : QByteArray();
}
QByteArray readHead(const QString &path) {
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return {};
    SafeFs::Fd guard(fd);
    return readHead(fd);
}
QByteArray linkText(const QString &path) {
    char buf[4096];
    const ssize_t n = ::readlink(QFile::encodeName(path).constData(), buf, sizeof(buf));
    return n > 0 && n < ssize_t(sizeof(buf)) ? QByteArray(buf, int(n)) : QByteArray();
}
// Byte-for-byte comparison of the open source file with a destination file of the same size.
bool sameContent(int sourceFd, const QString &destination) {
    QFile other(destination);
    if (!other.open(QIODevice::ReadOnly)) return false;
    constexpr qint64 kChunk = 1 << 20;
    QByteArray a(kChunk, Qt::Uninitialized);
    for (qint64 off = 0;;) {
        const ssize_t n = ::pread(sourceFd, a.data(), size_t(kChunk), off);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return false;
        const QByteArray b = other.read(n);
        if (b.size() != n || std::memcmp(a.constData(), b.constData(), size_t(n)) != 0) return false;
        if (n == 0) return true;
        off += n;
    }
}

class Walker {
public:
    Walker(const QString &volume, const QString &prefix, Plan *plan, const Cancelled &cancelled, QString *error)
        : volume(volume), prefix(prefix), plan(plan), cancelled(cancelled), error(error) {}

    bool run() {
        bool any = false;
        for (const char *rootName : kRoots) {
            const QString root = rootName;
            const QFileInfo source(volume + '/' + root);
            if (source.isSymLink() || !source.isDir()) continue; // a macOS without this directory is not an error by itself
            any = true;
            if (!checkDestinationParents(root)) return false;
            for (const QFileInfo &child : entries(root)) {
                if (cancelled && cancelled()) return fail(error, "Cancelled");
                const QString name = child.fileName();
                if (!name.endsWith(".framework")) continue;
                const QString rel = root + '/' + name;
                if (child.isSymLink()) { if (!symlink(rel, root)) return false; continue; }
                if (!child.isDir()) continue;
                if (!framework(rel, 0)) return false;
            }
        }
        if (!any) return fail(error, "This macOS source has no System/Library/Frameworks or System/Library/PrivateFrameworks directory. Nothing to import.");
        return true;
    }

private:
    const QString volume, prefix;
    Plan *plan;
    const Cancelled &cancelled;
    QString *error;

    QList<QFileInfo> entries(const QString &rel) const {
        return QDir(volume + '/' + rel).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
    }
    bool checkDestinationParents(const QString &root) const {
        QString current = prefix;
        for (const QString &part : root.split('/')) {
            current += '/' + part;
            if (QFileInfo(current).isSymLink()) return fail(error, "Destination parent is a symlink: " + part + " (the import never follows symlinks in the prefix)");
        }
        return true;
    }

    // Each add* decides against the prefix: absent means create, identical means present, anything else is a conflict.
    // Returns whether to descend: not into a directory the prefix has as a symlink or a file.
    bool addDir(const QString &rel) {
        const QFileInfo dest(prefix + '/' + rel);
        if (dest.isSymLink() || (dest.exists() && !dest.isDir())) { plan->conflicts << rel; return false; }
        if (!dest.exists()) { plan->entries.append({Kind::Directory, rel, 0, {}}); ++plan->directories; }
        return true;
    }
    bool file(const QString &rel) {
        if (hasSuffix(rel, kCodeFileSuffixes)) { ++plan->skippedByName; return true; }
        qint64 size = 0; QString err;
        SafeFs::Fd fd(SafeFs::openRead(volume, rel, &size, &err));
        if (fd.fd < 0) return fail(error, err);
        const QByteArray head = readHead(fd.fd);
        if (MachO::hasMachOMagic(head)) { ++plan->skippedMachO; return true; }
        if (head.startsWith("#!")) { ++plan->skippedScripts; return true; }
        const QFileInfo dest(prefix + '/' + rel);
        if (dest.isSymLink() || (dest.exists() && !dest.isFile())) plan->conflicts << rel;
        else if (!dest.exists()) { plan->entries.append({Kind::File, rel, size, {}}); ++plan->files; plan->bytes += quint64(size); }
        else if (dest.size() == size && sameContent(fd.fd, dest.filePath())) ++plan->alreadyPresent;
        else plan->conflicts << rel;
        return true;
    }
    // `root` is the bundle the link must stay inside.
    bool symlink(const QString &rel, const QString &root) {
        const QByteArray text = linkText(volume + '/' + rel);
        if (text.isEmpty() || text.startsWith('/')) { plan->refused << rel + ": absolute or unreadable symlink"; return true; }
        const QString resolved = QDir::cleanPath(rel.left(rel.lastIndexOf('/')) + '/' + QString::fromUtf8(text));
        const QString canonical = QFileInfo(volume + '/' + resolved).canonicalFilePath();
        const QString canonicalRoot = QFileInfo(volume + '/' + root).canonicalFilePath();
        if (!within(resolved, root)) { plan->refused << rel + ": symlink leaves the framework"; return true; }
        if (canonical.isEmpty()) { plan->refused << rel + ": dangling symlink"; return true; }
        if (!within(canonical, canonicalRoot)) { plan->refused << rel + ": symlink leaves the framework through another link"; return true; }
        if (QFileInfo(canonical).isFile()) { // a link to code is code
            const QByteArray head = readHead(canonical);
            if (MachO::hasMachOMagic(head)) { ++plan->skippedMachO; return true; }
            if (head.startsWith("#!")) { ++plan->skippedScripts; return true; }
        }
        const QFileInfo dest(prefix + '/' + rel);
        if (!dest.isSymLink() && !dest.exists()) {
            plan->entries.append({Kind::Symlink, rel, 0, QString::fromUtf8(text)}); ++plan->symlinks;
        } else if (dest.isSymLink() && linkText(dest.filePath()) == text) ++plan->alreadyPresent;
        else plan->conflicts << rel;
        return true;
    }
    bool resources(const QString &rel, const QString &root, int depth) {
        if (depth > kMaxDepth) return fail(error, "Directory nesting is too deep: " + rel);
        for (const QFileInfo &child : entries(rel)) {
            if (cancelled && cancelled()) return fail(error, "Cancelled");
            const QString name = child.fileName(), childRel = rel + '/' + name;
            if (child.isSymLink()) { if (!symlink(childRel, root)) return false; }
            else if (child.isDir()) {
                if (isCodeDir(name)) { ++plan->skippedByName; continue; }
                if (addDir(childRel) && !resources(childRel, root, depth + 1)) return false;
            }
            else if (child.isFile()) { if (!file(childRel)) return false; }
            else plan->refused << childRel + ": not a regular file";
        }
        return true;
    }
    bool framework(const QString &rel, int nesting) {
        ++plan->frameworks;
        if (!addDir(rel)) return true;
        const QString fwName = rel.mid(rel.lastIndexOf('/') + 1).chopped(int(strlen(".framework")));
        for (const QFileInfo &child : entries(rel)) {
            if (cancelled && cancelled()) return fail(error, "Cancelled");
            const QString name = child.fileName(), childRel = rel + '/' + name;
            if (name == "Versions" && child.isDir() && !child.isSymLink()) {
                if (addDir(childRel) && !versions(childRel, rel, fwName, nesting)) return false;
            } else if (!topLevelItem(child, childRel, rel, fwName, nesting)) return false;
        }
        return true;
    }
    bool versions(const QString &rel, const QString &root, const QString &fwName, int nesting) {
        for (const QFileInfo &child : entries(rel)) {
            const QString childRel = rel + '/' + child.fileName();
            if (child.isSymLink()) { if (!symlink(childRel, root)) return false; continue; }
            if (!child.isDir()) continue;
            if (!addDir(childRel)) continue;
            for (const QFileInfo &item : entries(childRel))
                if (!topLevelItem(item, childRel + '/' + item.fileName(), root, fwName, nesting)) return false;
        }
        return true;
    }
    // One entry of a framework directory or of Versions/<v>: only the resource parts are looked at.
    bool topLevelItem(const QFileInfo &item, const QString &rel, const QString &root, const QString &fwName, int nesting) {
        const QString name = item.fileName();
        const bool plain = !item.isSymLink();
        if (name == "Info.plist" || name == "version.plist") return plain ? (item.isFile() ? file(rel) : true) : symlink(rel, root);
        if (name == "Resources") {
            if (!plain) return symlink(rel, root);
            return !item.isDir() || !addDir(rel) || resources(rel, root, 0);
        }
        if (name == "Frameworks" && plain && item.isDir()) {
            if (!addDir(rel)) return true;
            for (const QFileInfo &nested : entries(rel)) {
                if (!nested.fileName().endsWith(".framework") || nested.isSymLink() || !nested.isDir()) continue;
                if (nesting + 1 > kMaxFrameworkNesting) { plan->refused << rel + '/' + nested.fileName() + ": frameworks nested too deeply"; continue; }
                if (!framework(rel + '/' + nested.fileName(), nesting + 1)) return false;
            }
            return true;
        }
        if (plain && item.isDir() && isCodeDir(name)) { ++plan->skippedByName; return true; }
        if (plain && item.isFile() && name == fwName) { // the framework binary, when the volume has one: counted, never copied
            const QByteArray head = readHead(item.filePath());
            if (MachO::hasMachOMagic(head)) ++plan->skippedMachO;
        }
        return true;
    }
};

}

bool buildPlan(const QString &volume, const QString &prefix, Plan *plan, QString *error, const Cancelled &cancelled) {
    *plan = Plan();
    if (!LauncherCore::validateLocations(volume, prefix, error)) return false;
    if (!Walker(volume, prefix, plan, cancelled, error).run()) return false;
    plan->availableBytes = QStorageInfo(prefix).bytesAvailable();
    return true;
}

QString formatBytes(quint64 bytes) { return QLocale::c().formattedDataSize(qint64(bytes), 1, QLocale::DataSizeSIFormat); }

QString summarize(const Plan &plan) {
    QString text;
    text += QString("Framework bundles scanned: %1\n").arg(plan.frameworks);
    text += QString("To copy: %1 files (%2), %3 directories, %4 symlinks\n").arg(plan.files).arg(formatBytes(plan.bytes)).arg(plan.directories).arg(plan.symlinks);
    text += QString("Already present and identical: %1\n").arg(plan.alreadyPresent);
    text += QString("Skipped as executable code, never copied: %1 Mach-O files, %2 scripts, %3 items by name (XPCServices, PlugIns, Helpers, .app, .dylib and similar)\n")
                .arg(plan.skippedMachO).arg(plan.skippedScripts).arg(plan.skippedByName);
    text += QString("Conflicts, left unchanged: %1\n").arg(plan.conflicts.size());
    for (const QString &item : plan.conflicts.mid(0, 10)) text += "  " + item + '\n';
    if (plan.conflicts.size() > 10) text += QString("  ... and %1 more\n").arg(plan.conflicts.size() - 10);
    text += QString("Refused source items: %1\n").arg(plan.refused.size());
    for (const QString &item : plan.refused.mid(0, 10)) text += "  " + item + '\n';
    if (plan.refused.size() > 10) text += QString("  ... and %1 more\n").arg(plan.refused.size() - 10);
    if (plan.availableBytes >= 0) text += QString("Free space on the prefix: %1%2\n").arg(formatBytes(quint64(plan.availableBytes)), plan.enoughSpace() ? "" : " (not enough)");
    return text;
}
}
