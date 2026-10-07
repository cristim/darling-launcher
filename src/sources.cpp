// SPDX-License-Identifier: GPL-3.0-or-later
#include "sources.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStorageInfo>
#include <algorithm>
namespace {
QString decode(const QByteArray &data) {
    QString value = QString::fromUtf8(data), result;
    for (int i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 3 < value.size()) {
            auto escaped = value.mid(i + 1, 3);
            if (escaped == "040" || escaped == "011" || escaped == "012" || escaped == "134") {
                result += QChar(escaped.toUInt(nullptr, 8)); i += 3; continue;
            }
        }
        result += value[i];
    }
    return result;
}
bool confinedDirectory(const QString &root, const QString &relative) {
    QFileInfo info(root + '/' + relative);
    const QString canonical = info.canonicalFilePath();
    return info.isDir() && info.isReadable() && canonical.startsWith(root + '/');
}
SourceCandidate inspect(const SourceMount &mount, const QString &path) {
    SourceCandidate candidate{mount.path, path, mount.device, false, false, false, mount.readOnly};
    const QString canonical = QFileInfo(path).canonicalFilePath(), parent = QFileInfo(mount.path).canonicalFilePath();
    if (canonical.isEmpty() || parent.isEmpty() || (canonical != parent && !canonical.startsWith(parent + '/'))) return candidate;
    candidate.readable = QFileInfo(path).isDir() && QDir(path).isReadable();
    if (candidate.readable) {
        candidate.apps = confinedDirectory(canonical, "Applications") || confinedDirectory(canonical, "System/Applications");
        candidate.libraries = confinedDirectory(canonical, "System/Library") || confinedDirectory(canonical, "usr/lib") || confinedDirectory(canonical, "Library/Frameworks");
    }
    return candidate;
}
}
QString SourceCandidate::description() const {
    QString status = !readable ? "Not readable" : apps && libraries ? "Apps and libraries" : apps ? "Apps only" : libraries ? "Libraries only" : "No app/library directories";
    return device + " — " + root + " — " + status + (readOnly ? " (read-only)" : "");
}
QList<SourceMount> LauncherSources::parseMountInfo(const QByteArray &data) {
    QList<SourceMount> result;
    for (const auto &line : data.split('\n')) {
        const auto fields = line.split(' '); const int separator = fields.indexOf("-");
        if (separator < 6 || fields.size() <= separator + 3) continue;
        result.append({decode(fields[4]), decode(fields[separator + 2]), decode(fields[separator + 1]), fields[5].split(',').contains("ro")});
    }
    return result;
}
QList<SourceMount> LauncherSources::mounts() {
    QFile file("/proc/self/mountinfo");
    if (file.open(QIODevice::ReadOnly)) return parseMountInfo(file.readAll());
    QList<SourceMount> result;
    for (const auto &storage : QStorageInfo::mountedVolumes())
        if (storage.isValid() && storage.isReady()) result.append({storage.rootPath(), QString::fromLocal8Bit(storage.device()), QString::fromLocal8Bit(storage.fileSystemType()), storage.isReadOnly()});
    return result;
}
QList<SourceCandidate> LauncherSources::candidates(const QList<SourceMount> &records, const QString &device) {
    QList<SourceCandidate> result; QStringList seen;
    for (const auto &mount : records) {
        if (mount.path == "/" || mount.path.isEmpty() || (!device.isEmpty() && mount.device != device)) continue;
        const bool macFilesystem = mount.filesystem == "apfs" || mount.filesystem.startsWith("hfs") || mount.filesystem.startsWith("fuse.apfs") || (mount.filesystem == "fuse" && mount.device.startsWith("/dev/"));
        if (device.isEmpty() && !macFilesystem) continue;
        auto candidate = inspect(mount, mount.path);
        // Some APFS FUSE versions expose the volume filesystem under /root.
        if (!candidate.usable()) {
            auto wrapped = inspect(mount, mount.path + "/root");
            if (wrapped.usable()) candidate = wrapped;
        }
        if (!seen.contains(candidate.root)) { seen << candidate.root; result << candidate; }
    }
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.root < b.root; });
    return result;
}
QString LauncherSources::automaticSource(const QList<SourceCandidate> &items, const QString &current) {
    QStringList usable;
    for (const auto &item : items) if (item.usable()) usable << item.root;
    if (usable.contains(current)) return current;
    return usable.size() == 1 ? usable.first() : current;
}
