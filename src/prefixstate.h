// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>
#include <functional>

// Is the Darling server for a prefix up? While it runs, the prefix path is the overlay's merged view,
// so the upper layer must not be touched. Mirrors darling.c getInitProcess and darlingserver's socket path.
namespace PrefixState {
// True unless a connect() to the socket definitively fails (absent file or ECONNREFUSED).
bool socketMayBeLive(const QString &socketPath);

struct ProcFs {
    QString procRoot = "/proc";
    std::function<bool(const QString &socketPath)> socketMayBeLive = &PrefixState::socketMayBeLive;
};

// Fails closed: anything other than "no live socket and no pid file, or a pid file whose
// /proc/<pid> is definitively absent" counts as running, and `why` says which doubt applied.
bool prefixRunning(const QString &prefix, const ProcFs &procFs = {}, QString *why = nullptr);
}
