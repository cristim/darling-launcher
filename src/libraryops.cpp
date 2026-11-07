// SPDX-License-Identifier: GPL-3.0-or-later
#include "libraryops.h"
#include "core.h"
#include "imagesource.h"

namespace {
using LauncherCore::fail;
int indexOf(const LibrariesFile &file, const QString &name) {
    for (int i = 0; i < file.libraries.size(); ++i) if (file.libraries[i].installName == name) return i;
    return -1;
}
bool upperState(const QString &prefix, const LibrariesFile &file, const QString &name, const ImportOptions &o, UpperState *st, QString *error) {
    const int i = indexOf(file, name);
    return upperFileState(prefix, name, i >= 0 ? &file.libraries[i] : nullptr, o.maxImageBytes, st, error);
}
bool notOurs(const QString &name, QString *error) {
    return fail(error, "Refusing to touch a file this launcher did not write (it was modified or is not ours): " + name);
}
// Bytes of the recorded image, from the live store copy or a pending one, verified against the recorded sha256.
bool recordedBytes(const QString &prefix, const LibraryRecord &r, qint64 maxBytes, QByteArray *out, QString *error) {
    const QString dir = ".darling-launcher/libs/" + r.id;
    for (const char *name : {"/image", "/image.pending"}) {
        bool exists = false;
        if (!SafeFs::readFile(prefix, dir + name, maxBytes, out, &exists, error)) return false;
        if (exists && ImageSources::sha256Hex(*out) == r.source.sha256) return true;
    }
    return fail(error, "No store copy matches the recorded sha256 for " + r.installName);
}
// With skipped set, a file that is not ours is left alone and reported there instead of refused.
bool removeOurs(const QString &prefix, const LibrariesFile &file, const QString &name, const ImportOptions &o, bool dryRun,
                QStringList *skipped, QString *error) {
    UpperState st;
    if (!upperState(prefix, file, name, o, &st, error)) return false;
    if (!st.exists) return true;
    if (!st.ours) {
        if (!skipped) return notOurs(name, error);
        if (!dryRun) skipped->append(name);
        return true;
    }
    return dryRun || SafeFs::removeFile(prefix, name.mid(1), error);
}
// Brings the upper file to the recorded content; a file that is not ours is never overwritten.
bool materialize(const QString &prefix, const LibrariesFile &file, const LibraryRecord &r, const ImportOptions &o, bool dryRun, QString *error) {
    UpperState st;
    if (!upperState(prefix, file, r.installName, o, &st, error)) return false;
    if (st.exists && !st.ours) return notOurs(r.installName, error);
    if (st.exists && st.sha == r.source.sha256) return true;
    QByteArray bytes;
    if (!recordedBytes(prefix, r, o.maxImageBytes, &bytes, error)) return false;
    return dryRun || SafeFs::writeFile(prefix, r.installName.mid(1), bytes, 0644, 0755, st.exists, error);
}
bool aborted(const ImportOptions &o, const QString &stage, QString *error) {
    if (!o.abortAfter || !o.abortAfter(stage)) return false;
    fail(error, "Aborted after " + stage);
    return true;
}
}

bool disableLibrary(const QString &prefix, const QString &installName, QString *error, const ImportOptions &options) {
    PrefixSession s;
    if (!openSession(prefix, options, &s, error)) return false;
    const int i = indexOf(s.file, installName);
    UpperState st;
    if (!upperState(prefix, s.file, installName, options, &st, error)) return false;
    // Idempotent: nothing of ours to remove is success. Only an enabled record whose file was tampered with is an error.
    if (st.exists && !st.ours && i >= 0 && s.file.libraries[i].enabled) return notOurs(installName, error);
    if (st.exists && st.ours && !SafeFs::removeFile(prefix, installName.mid(1), error)) return false;
    if (aborted(options, "removed", error)) return false;
    if (i < 0 || !s.file.libraries[i].enabled) return true;
    s.file.libraries[i].enabled = false;
    return Libraries::save(prefix, s.file, error);
}

bool abandonLibrary(const QString &prefix, const QString &installName, QString *error, const ImportOptions &options) {
    PrefixSession s;
    if (!openSession(prefix, options, &s, error)) return false;
    const int i = indexOf(s.file, installName);
    UpperState st;
    if (!upperState(prefix, s.file, installName, options, &st, error)) return false;
    if (st.exists && st.ours) return fail(error, "The upper file is still ours; disable the library instead of abandoning it: " + installName);
    if (i >= 0) {
        s.file.libraries.removeAt(i);
        if (!Libraries::save(prefix, s.file, error)) return false;
    }
    // Only files this launcher created; the upper file is never touched.
    const QString dir = ".darling-launcher/libs/" + Libraries::idFor(installName);
    return SafeFs::removeFile(prefix, dir + "/image", error) && SafeFs::removeFile(prefix, dir + "/image.pending", error)
        && SafeFs::removeDirectory(prefix, dir, error);
}

bool enableLibrary(const QString &prefix, const QString &runtimeRoot, const QString &installName, bool force,
                   QString *error, const ImportOptions &options) {
    PrefixSession s;
    if (!openSession(prefix, options, &s, error)) return false;
    const int i = indexOf(s.file, installName);
    if (i < 0) return fail(error, "Library is not imported: " + installName);
    if (!s.file.enabled) return fail(error, "Libraries are switched off for this prefix; switch them on first");
    if (!checkRuntimeAllows(runtimeRoot, installName, force, error)) return false;
    if (!materialize(prefix, s.file, s.file.libraries[i], options, false, error)) return false;
    if (aborted(options, "upper", error)) return false;
    if (s.file.libraries[i].enabled) return true;
    s.file.libraries[i].enabled = true;
    return Libraries::save(prefix, s.file, error);
}

bool setMasterSwitch(const QString &prefix, const QString &runtimeRoot, bool enabled, bool force, QStringList *skipped,
                     QString *error, const ImportOptions &options) {
    PrefixSession s;
    if (!openSession(prefix, options, &s, error)) return false;
    if (skipped) skipped->clear();
    if (s.file.enabled == enabled) return true;
    QStringList left;
    for (bool dryRun : {true, false}) { // check every library first so a refusal changes nothing
        for (const LibraryRecord &r : s.file.libraries) {
            if (!r.enabled) continue;
            if (enabled && dryRun && !checkRuntimeAllows(runtimeRoot, r.installName, force, error)) return false;
            if (!(enabled ? materialize(prefix, s.file, r, options, dryRun, error) : removeOurs(prefix, s.file, r.installName, options, dryRun, &left, error))) return false;
            if (!dryRun && aborted(options, "item:" + r.installName, error)) return false;
        }
    }
    if (aborted(options, enabled ? "upper" : "removed", error)) return false;
    s.file.enabled = enabled;
    if (!Libraries::save(prefix, s.file, error)) return false;
    if (skipped) *skipped = left;
    return true;
}
