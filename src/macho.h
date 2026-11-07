// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>

// Minimal reader for the dependent-library names of a 64-bit arm64 Mach-O image (thin or fat).
// Input is untrusted: every size and offset is bounded and rejected loudly.
namespace MachO {
constexpr quint32 kSubtypeArm64 = 0;
constexpr quint32 kSubtypeArm64e = 2;

struct Dependency {
    QString installName;
    bool weak = false; // LC_LOAD_WEAK_DYLIB: the image still loads when this one is absent
};
struct Dependencies {
    QList<Dependency> dependencies; // in load-command order, duplicates kept
    quint32 subtype = 0;      // masked cpusubtype of the slice that was read
    bool usedFallback = false; // the preferred subtype was absent and another arm64 slice was read
};

// Picks the arm64 slice whose masked cpusubtype == preferredSubtype. Another arm64 slice is used
// only when allowFallback is set.
bool readDependencies(const QByteArray &image, quint32 preferredSubtype, bool allowFallback,
                      Dependencies *out, QString *error);

using Resolver = std::function<std::optional<QByteArray>(const QString &installName)>;
struct Closure {
    QStringList installNames;       // breadth-first discovery order, root excluded
    QStringList unresolved;         // absolute names the resolver had no image for
    QStringList unresolvableRelative; // @rpath/@loader_path/@executable_path and other non-absolute names
};
// Walks the dependency graph from root. Fails when a node or depth cap is hit or any image is malformed.
bool dependencyClosure(const QByteArray &root, const Resolver &resolve, quint32 preferredSubtype,
                       bool allowFallback, Closure *out, QString *error);
}
