// SPDX-License-Identifier: GPL-3.0-or-later
// Shared fixtures for the import, enable/disable and status tests (synthetic files in temporary directories).
#pragma once
#include "imagesource.h"
#include "libraryimport.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <sys/stat.h>
#include <unistd.h>

inline const QString kLib = "/usr/lib/libfoo.dylib";
inline const QString kAppKit = "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit";

static inline QByteArray slurp(const QString &path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }
static inline void put(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(data);
}
static inline LibrarySource srcFor(const QByteArray &b) { return {"fixture", "uuid", "tool", ImageSources::sha256Hex(b)}; }
static inline int modeOf(const QString &p) { struct stat st; return ::stat(p.toUtf8().constData(), &st) == 0 ? int(st.st_mode & 0777) : -1; }

struct Env {
    QTemporaryDir prefix, runtime, proc, evil;
    ImportOptions opts;
    Env() { opts.procFs.procRoot = proc.path(); opts.procFs.socketMayBeLive = [](const QString &) { return false; }; }
    bool import(const QString &name, const QByteArray &b, bool force = false, QString *err = nullptr) {
        QString local; return importImage(prefix.path(), runtime.path(), name, b, srcFor(b), force, err ? err : &local, opts);
    }
    QString upper(const QString &name = kLib) const { return prefix.path() + name; }
    LibrariesFile records() { LibrariesFile f; QString e; Libraries::load(prefix.path(), &f, &e); return f; }
    bool untouched() const { return !QFileInfo::exists(prefix.path() + "/.darling-launcher") && !QFileInfo::exists(upper()) && QDir(evil.path()).isEmpty(); }
};

