// SPDX-License-Identifier: GPL-3.0-or-later
#include "libraryimport.h"
#include "core.h"
#include "imagesource.h"
#include <QFileInfo>
#include <QRegularExpression>
#include <unistd.h>

namespace {
using LauncherCore::fail;
struct LockGuard { int fd; ~LockGuard() { if (fd >= 0) ::close(fd); } };
QString storeDir(const QString &id) { return ".darling-launcher/libs/" + id; }

}

// Same collapse as LauncherCore::importLibrary: Foo.framework/Versions/A/Foo is protected as Foo.framework.
bool runtimeProvides(const QString &runtimeRoot, const QString &installName) {
    QString relative = installName.mid(1);
    static const QRegularExpression framework(R"((^|/)([^/]+\.framework)(/|$))");
    const auto match = framework.match(relative);
    if (match.hasMatch()) relative = relative.left(match.capturedEnd(2));
    const QFileInfo provided(runtimeRoot + "/" + relative);
    return provided.exists() || provided.isSymLink();
}

bool checkRuntimeAllows(const QString &runtimeRoot, const QString &installName, bool force, QString *error) {
    if (Libraries::isPinned(installName) && !force) return fail(error, "Refusing to shadow a library Darling ships itself: " + installName);
    const QFileInfo root(runtimeRoot);
    if (!root.isAbsolute() || !root.isDir()) return fail(error, "The runtime root must be an existing absolute directory: " + runtimeRoot);
    if (runtimeProvides(runtimeRoot, installName) && !force) return fail(error, "The Darling runtime already provides " + installName);
    return true;
}

bool upperFileState(const QString &prefix, const QString &installName, const LibraryRecord *record, qint64 maxBytes,
                    UpperState *out, QString *error) {
    UpperState st; QByteArray bytes, pending; bool pendingExists = false;
    if (!SafeFs::readFile(prefix, installName.mid(1), maxBytes, &bytes, &st.exists, error)) return false;
    if (!SafeFs::readFile(prefix, storeDir(Libraries::idFor(installName)) + "/image.pending", maxBytes, &pending, &pendingExists, error)) return false;
    if (pendingExists) st.pendingSha = ImageSources::sha256Hex(pending);
    if (st.exists) {
        st.sha = ImageSources::sha256Hex(bytes);
        const bool recorded = record && record->source.sha256 == st.sha;
        st.ours = recorded || (pendingExists && st.sha == st.pendingSha);
        st.interrupted = st.ours && !recorded;
    }
    *out = st;
    return true;
}

bool importImage(const QString &prefix, const QString &runtimeRoot, const QString &installName, const QByteArray &bytes,
                 const LibrarySource &source, bool force, QString *error, const ImportOptions &options) {
    if (bytes.size() > options.maxImageBytes) return fail(error, "Image is too large");
    FetchedImage image{bytes, source};
    if (!ImageSources::verifyFetched(installName, &image, error)) return false; // also validates the install name
    LibraryRecord record{installName, Libraries::idFor(installName), true, image.source};
    if (!Libraries::validRecord(record, error)) return false;
    if (!checkRuntimeAllows(runtimeRoot, installName, force, error)) return false;

    LockGuard lock{SafeFs::lockFile(prefix, ".darling-launcher/lock", 0755, error)};
    if (lock.fd < 0) return false;
    QString why;
    if (PrefixState::prefixRunning(prefix, options.procFs, &why)) return fail(error, "Refusing to import while the prefix may be running: " + why);
    LibrariesFile file;
    if (!Libraries::load(prefix, &file, error)) return false;
    if (!file.enabled && !options.overrideMasterSwitch) return fail(error, "Library imports are switched off for this prefix (libraries.json enabled=false)");
    int recordIndex = -1;
    for (int i = 0; i < file.libraries.size(); ++i) if (file.libraries[i].installName == installName) recordIndex = i;

    UpperState upper;
    if (!upperFileState(prefix, installName, recordIndex >= 0 ? &file.libraries[recordIndex] : nullptr, options.maxImageBytes, &upper, error)) return false;
    if (upper.exists && !upper.ours) return fail(error, "Refusing to overwrite a file this launcher did not import: " + installName);
    // An interrupted replace left a file that equals the pending image: provably ours, so any new image may replace it.
    // Remove it first so every later crash point still leaves either no upper file or a recognisable one.
    if (upper.interrupted && upper.sha != record.source.sha256) {
        if (!SafeFs::removeFile(prefix, installName.mid(1), error)) return false;
    }

    // Validate the updated records (count, size) now, before anything is written.
    LibrariesFile updated = file;
    if (recordIndex >= 0) updated.libraries[recordIndex] = record; else updated.libraries.append(record);
    QByteArray json;
    if (!Libraries::serialize(updated, &json, error)) return false;

    const QString dir = storeDir(record.id);
    if (!SafeFs::writeFile(prefix, dir + "/image.pending", bytes, 0600, 0700, true, error)) return false;
    if (options.abortAfter && options.abortAfter("pending")) return fail(error, "Import aborted after the pending write");
    if (!(upper.exists && upper.sha == record.source.sha256)) {
        if (!SafeFs::writeFile(prefix, installName.mid(1), bytes, 0644, 0755, upper.exists, error, options.upperWriteHooks)) return false;
    }
    if (options.abortAfter && options.abortAfter("upper")) return fail(error, "Import aborted after the upper write");
    if (!SafeFs::writeFile(prefix, ".darling-launcher/libraries.json", json, 0644, 0755, true, error)) return false;
    if (options.abortAfter && options.abortAfter("record")) return fail(error, "Import aborted after the record write");
    return SafeFs::renameInDir(prefix, dir, "image.pending", "image", error);
}
