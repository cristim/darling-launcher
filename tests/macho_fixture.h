// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic Mach-O builders shared by tests; no Apple files are read.
#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

static inline void put32(QByteArray &b, quint32 v, bool be = false) {
    for (int i = 0; i < 4; ++i) b.append(char((be ? v >> (24 - 8 * i) : v >> (8 * i)) & 0xff));
}
static inline void patch32(QByteArray &b, int off, quint32 v, bool be = false) {
    QByteArray t; put32(t, v, be); b.replace(off, 4, t);
}
// Well-formed dylib command: header(24) + name + NUL, padded to a multiple of 8.
static inline QByteArray dylibCmd(const QString &name, quint32 cmd = 0xc) {
    QByteArray n = name.toUtf8() + '\0';
    while ((24 + n.size()) % 8) n.append('\0');
    QByteArray c; put32(c, cmd); put32(c, 24 + n.size()); put32(c, 24); put32(c, 0); put32(c, 0); put32(c, 0);
    return c + n;
}
static inline QByteArray thinWith(quint32 subtype, const QList<QByteArray> &cmds, quint32 cpu = 0x0100000c) {
    QByteArray body; for (const auto &c : cmds) body += c;
    QByteArray b; put32(b, 0xfeedfacf); put32(b, cpu); put32(b, subtype); put32(b, 2); put32(b, cmds.size());
    put32(b, body.size()); put32(b, 0); put32(b, 0);
    return b + body;
}
static inline QByteArray thin(quint32 subtype, const QStringList &names) {
    QList<QByteArray> cmds; for (const auto &n : names) cmds << dylibCmd(n);
    return thinWith(subtype, cmds);
}
