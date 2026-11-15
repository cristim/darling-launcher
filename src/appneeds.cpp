// SPDX-License-Identifier: GPL-3.0-or-later
#include "appneeds.h"
#include "core.h"
#include "libraries.h"
#include "safefs.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStorageInfo>

namespace AppNeeds {
namespace {
using LauncherCore::fail;
constexpr int kMaxImages = 64; // the executable plus embedded libraries
const QStringList kEmbeddedPrefixes{"@rpath/", "@executable_path/../Frameworks/"};
const QRegularExpression &frameworkName() {
    static const QRegularExpression re(R"(^/System/Library/(Frameworks|PrivateFrameworks)/([^/]+)\.framework/)");
    return re;
}
bool isRegularFile(const QString &path) { const QFileInfo info(path); return info.isFile() && !info.isSymLink(); }
bool isRealDir(const QString &path) { const QFileInfo info(path); return info.isDir() && !info.isSymLink(); }
QString clean(QString text) {
    text.replace(QRegularExpression("[^A-Za-z0-9 ._,:+()/=-]"), "?");
    return text.left(240);
}
}

NameReader windowedReader() {
    return [](const QString &volume, const QString &rel, QList<MachO::Dependency> *names, QString *error) {
        qint64 size = 0;
        SafeFs::Fd fd(SafeFs::openRead(volume, rel, &size, error));
        if (fd.fd < 0) return false;
        const MachO::ReadAt read = [&](quint64 offset, quint64 length, QByteArray *out) {
            out->resize(qsizetype(length));
            return SafeFs::preadFull(fd.fd, out->data(), qint64(length), qint64(offset));
        };
        MachO::Dependencies deps;
        if (!MachO::readDependenciesWindowed(read, quint64(size), MachO::kSubtypeArm64e, true, &deps, error)) return false;
        *names = deps.dependencies;
        return true;
    };
}

bool linkedNames(const QString &volume, const QString &appBundleRel, Names *out, QString *error, const NameReader &read) {
    if (!LauncherCore::confinedRelative(appBundleRel) || !appBundleRel.endsWith(".app")) return fail(error, "Not an app bundle path below the volume: " + appBundleRel);
    const QString plist = volume + '/' + appBundleRel + "/Contents/Info.plist";
    if (!isRegularFile(plist) || !LauncherCore::within(QFileInfo(plist).canonicalFilePath(), QFileInfo(volume).canonicalFilePath()))
        return fail(error, "The app's Info.plist is missing, a link, or outside the volume");
    const QString executable = LauncherCore::infoPlistString(plist, "CFBundleExecutable");
    if (executable.isEmpty() || !LauncherCore::confinedRelative(executable) || executable.contains('/')) return fail(error, "The app has no valid CFBundleExecutable");
    Names names;
    names.appName = QFileInfo(appBundleRel).completeBaseName();
    names.executable = executable;
    names.executableRel = appBundleRel + "/Contents/MacOS/" + executable;
    struct Image { QString rel, via; };
    QList<Image> queue{{names.executableRel, {}}};
    QSet<QString> seenImages{names.executableRel}, seenNames, seenRelative;
    for (int i = 0; i < queue.size(); ++i) {
        if (i >= kMaxImages) return fail(error, "The app embeds more than " + QString::number(kMaxImages) + " libraries; refusing to follow them");
        QList<MachO::Dependency> deps; QString why;
        if (!read(volume, queue[i].rel, &deps, &why)) {
            if (i == 0) return fail(error, queue[i].rel + ": " + why);
            names.unresolvedRelative << queue[i].via + " (not readable: " + why + ")"; // an embedded image that is no Mach-O or sits behind a link
            continue;
        }
        if (i > 0) names.embeddedRead << queue[i].rel;
        for (const MachO::Dependency &dep : deps) {
            const QString &name = dep.installName;
            if (name.startsWith('/')) {
                if (seenNames.contains(name)) continue;
                seenNames.insert(name); names.linked.append({name, queue[i].via});
                continue;
            }
            QString tail;
            for (const QString &prefix : kEmbeddedPrefixes) if (name.startsWith(prefix)) { tail = name.mid(prefix.size()); break; }
            const QString rel = appBundleRel + "/Contents/Frameworks/" + tail;
            if (!tail.isEmpty() && LauncherCore::confinedRelative(tail) && isRegularFile(volume + '/' + rel)) {
                if (!seenImages.contains(rel)) { seenImages.insert(rel); queue.append({rel, rel}); }
            } else if (!seenRelative.contains(name)) { seenRelative.insert(name); names.unresolvedRelative << name; }
        }
    }
    *out = names;
    return true;
}

int Plan::selectedCount() const { return int(byStatus(Status::Selected).size()); }
int Plan::selectedFiles() const { int n = 0; for (const Item *it : byStatus(Status::Selected)) n += it->files; return n; }
quint64 Plan::selectedBytes() const { quint64 n = 0; for (const Item *it : byStatus(Status::Selected)) n += it->bytes; return n; }
QStringList Plan::selectedNames() const { QStringList n; for (const Item *it : byStatus(Status::Selected)) n << it->name; return n; }
QList<const Item *> Plan::byStatus(Status status) const {
    QList<const Item *> list;
    for (const Item &it : items) if (it.status == status) list << &it;
    return list;
}

namespace {
// The app's own family: a framework named after the app (Notes -> NotesShared, NotesUI, ...).
bool inFamily(const QString &framework, const Names &names) {
    for (const QString &base : {names.appName, names.executable})
        if (base.size() >= 4 && framework.size() > base.size() && framework.startsWith(base)) return true; // a short name would match unrelated frameworks
    return false;
}
}

bool buildPlan(const QString &volume, const QString &prefix, const QString &appBundleRel, const Options &options, Plan *plan, QString *error) {
    if (!LauncherCore::validateLocations(volume, prefix, error)) return false;
    Names names;
    if (!linkedNames(volume, appBundleRel, &names, error, options.reader ? options.reader : windowedReader())) return false;
    Plan result;
    result.appName = names.appName; result.appBundleRel = appBundleRel; result.executableRel = names.executableRel;
    result.unresolvedRelative = names.unresolvedRelative; result.linkedCount = int(names.linked.size());
    QSet<QString> seen;
    for (const Linked &link : names.linked) result.linkedInstallNames << link.installName;
    for (const Linked &link : names.linked) {
        const QRegularExpressionMatch match = frameworkName().match(link.installName);
        if (!match.hasMatch()) { result.notFrameworkBundles << link.installName; continue; }
        const QString root = match.captured(1), name = match.captured(2);
        if (seen.contains(name)) continue;
        seen.insert(name);
        Item item; item.name = name; item.installName = link.installName; item.via = link.via;
        const bool inFrameworks = isRealDir(volume + "/System/Library/Frameworks/" + name + ".framework");
        const bool inPrivate = isRealDir(volume + "/System/Library/PrivateFrameworks/" + name + ".framework");
        if (Libraries::isPinned(link.installName)) item.reason = "provided by Darling";
        else if (!(root == "Frameworks" ? inFrameworks : inPrivate)) item.reason = "not on the macOS volume";
        else if (inFrameworks && inPrivate) item.reason = "the name exists in both Frameworks and PrivateFrameworks (use frameworks-import --only)";
        else {
            FrameworkResources::Plan fw; QString why;
            if (!FrameworkResources::buildPlan(volume, prefix, &fw, &why, {}, {name})) return fail(error, name + ": " + why);
            item.files = fw.files; item.directories = fw.directories; item.symlinks = fw.symlinks; item.bytes = fw.bytes; item.apparentBytes = fw.apparentBytes;
            if (fw.files == 0) { // a Versions/Current link alone is not a resource
                item.reason = fw.alreadyPresent > 0 ? "resources already present and identical"
                              : !fw.conflicts.isEmpty() ? "the prefix has different files at the same paths (left unchanged)"
                                                        : "no resource files to copy";
            } else if (inFamily(name, names)) { item.status = Status::Selected; item.reason = "part of the app's own family"; }
            else { item.status = Status::Optional; item.reason = "linked; not selected unless you choose it"; }
        }
        result.items << item;
    }
    for (const QString &wanted : options.also) {
        Item *found = nullptr;
        for (Item &item : result.items) if (item.name == wanted) found = &item;
        if (!found) return fail(error, wanted + " is not a framework this app links, so it is not imported");
        if (found->status == Status::Skipped) return fail(error, wanted + " cannot be imported: " + found->reason);
        if (found->status == Status::Optional) { found->status = Status::Selected; found->reason = "chosen by you"; }
    }
    result.availableBytes = options.freeBytes ? options.freeBytes(prefix) : QStorageInfo(prefix).bytesAvailable();
    *plan = result;
    return true;
}

QString summarize(const Plan &plan) {
    using FrameworkResources::formatBytes;
    QString text = QString("App: %1 (%2 linked names, %3 framework bundles)\n").arg(plan.appName).arg(plan.linkedCount).arg(plan.items.size());
    auto section = [&](const QString &title, Status status) {
        const auto list = plan.byStatus(status);
        text += QString("%1: %2\n").arg(title).arg(list.size());
        for (const Item *it : list) {
            if (status == Status::Skipped) text += QString("  %1: %2\n").arg(it->name, it->reason);
            else text += QString("  %1: %2 files, %3 directories, %4 symlinks, %5 on disk (%6)\n").arg(it->name).arg(it->files).arg(it->directories).arg(it->symlinks).arg(formatBytes(it->bytes), it->reason);
        }
    };
    section("Selected framework resources", Status::Selected);
    section("Optional (linked, not selected)", Status::Optional);
    section("Skipped", Status::Skipped);
    if (!plan.notFrameworkBundles.isEmpty()) text += QString("Linked system libraries that are not framework bundles: %1\n").arg(plan.notFrameworkBundles.size());
    if (!plan.unresolvedRelative.isEmpty()) text += QString("Linked relative names with no file in the app: %1\n").arg(plan.unresolvedRelative.size());
    text += QString("Total to copy: %1 files, about %2 on disk\n").arg(plan.selectedFiles()).arg(formatBytes(plan.selectedBytes()));
    if (plan.availableBytes >= 0) text += QString("Free space on the prefix: %1\n").arg(formatBytes(quint64(plan.availableBytes)));
    text += "Only resource files are copied, never executable code; nothing in the prefix is replaced.\n";
    return text;
}

QString logPath(const QString &prefix) { return prefix + "/.darling-launcher/app-imports.md"; }

bool appendLog(const QString &prefix, const QString &app, const QString &step, const QString &items, const QString &counts, const QString &note, QString *error) {
    const QString rel = ".darling-launcher/app-imports.md";
    QByteArray data; bool exists = false;
    if (!SafeFs::readFile(prefix, rel, 1 << 20, &data, &exists, error)) return false;
    if (!exists) data = "# App imports\n\nOne row per import step: what was copied from the user's own macOS volume, and when.\n\n| time (UTC) | app | step | items | counts | note |\n|---|---|---|---|---|---|\n";
    data += QString("| %1 | %2 | %3 | %4 | %5 | %6 |\n").arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate), clean(app), clean(step), clean(items), clean(counts), clean(note)).toUtf8();
    return SafeFs::writeFile(prefix, rel, data, 0644, 0755, true, error);
}

bool applyResources(const QString &volume, const QString &prefix, const Plan &plan, bool confirmed, const Options &options, Applied *applied,
                    QString *error, const FrameworkResources::Progress &progress, const FrameworkResources::Cancelled &cancelled) {
    *applied = {};
    if (!confirmed) return fail(error, "Confirmation is required: nothing was copied.");
    const QStringList names = plan.selectedNames();
    if (names.isEmpty()) return true;
    FrameworkResources::Plan fw;
    if (!FrameworkResources::buildPlan(volume, prefix, &fw, error, cancelled, names)) return false;
    if (options.freeBytes) fw.availableBytes = options.freeBytes(prefix);
    if (!fw.enoughSpace())
        return fail(error, "Not enough free space: the resources need about " + FrameworkResources::formatBytes(fw.bytes) + " plus a 512 MiB reserve, and the prefix has "
                               + (fw.availableBytes < 0 ? QString("an unknown amount") : FrameworkResources::formatBytes(quint64(fw.availableBytes))) + " free. Nothing was copied.");
    applied->bundles = names;
    if (fw.entries.isEmpty()) return true;
    if (!FrameworkResources::applyPlan(volume, prefix, fw, &applied->result, error, progress, cancelled)) return false;
    const FrameworkResources::Result &r = applied->result;
    QString logError;
    if (!appendLog(prefix, plan.appName, "framework resources", names.join(", "),
                     QString("%1 files, %2 bytes, %3 directories, %4 symlinks").arg(r.files).arg(r.bytes).arg(r.directories).arg(r.symlinks),
                     QString("%1 cloned, %2 appeared meanwhile").arg(r.reflinked).arg(r.appeared), &logError))
        return fail(error, "The resources were copied, but the provenance log could not be updated: " + logError);
    return true;
}
}
