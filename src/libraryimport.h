// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "libraries.h"
#include "prefixstate.h"
#include "safefs.h"
#include <QByteArray>
#include <QString>
#include <functional>

struct ImportOptions {
    PrefixState::ProcFs procFs;
    SafeFs::Hooks upperWriteHooks; // test seam for the descriptor-chain write of the upper file
    // Test seam: called after the "pending", "upper" and "record" steps; returning true aborts the import there.
    std::function<bool(const QString &stage)> abortAfter;
    // libraries.json enabled=false is the per-prefix master switch; importing into a switched-off prefix needs this.
    bool overrideMasterSwitch = false;
    qint64 maxImageBytes = qint64(512) << 20;
};

// State of <prefix>/<installName> relative to what this launcher wrote. A file is "ours" when its sha256 equals the
// recorded one, or equals the sha256 of the pending store file an interrupted replace leaves behind.
struct UpperState {
    bool exists = false;
    bool ours = false;
    bool interrupted = false; // ours only through the pending file: a replace was cut short
    QString sha;
    QString pendingSha;
};
bool upperFileState(const QString &prefix, const QString &installName, const LibraryRecord *record, qint64 maxBytes,
                    UpperState *out, QString *error);

// The "builtin" rule: does the Darling runtime already provide this path (or its framework directory)?
bool runtimeProvides(const QString &runtimeRoot, const QString &installName);
// Refuses pinned paths and runtime-provided paths unless force; runtimeRoot must be an existing absolute directory.
bool checkRuntimeAllows(const QString &runtimeRoot, const QString &installName, bool force, QString *error);

// Copies one library image into the overlay upper layer (<prefix>/<installName>), keeps the canonical copy in
// <prefix>/.darling-launcher/libs/<id>/image and records it in libraries.json.
// runtimeRoot is darlingserver's libexec path (Config::getLibexecPath): a path it already provides is refused
// unless force. The lock excludes only other launcher processes (best effort), not Darling startup.
// Every crash point is recoverable. Order: write image.pending, write the upper file, record the new sha256,
// rename image.pending over image. Re-importing completes an interrupted import, with the same or a different image.
bool importImage(const QString &prefix, const QString &runtimeRoot, const QString &installName, const QByteArray &bytes,
                 const LibrarySource &source, bool force, QString *error, const ImportOptions &options = {});
