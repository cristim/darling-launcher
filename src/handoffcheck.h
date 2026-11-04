// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QList>
#include <QString>
#include <QStringList>
#include <optional>
// Native port of the clean-room handoff checker (reference oracle: check_handoff.py, kept as a
// test oracle in tests/handoff-oracle). A tripwire for disassembly-shaped text, not a proof.
namespace HandoffCheck {
struct Hit {
    int line = 0;
    QString rule, description;
    QString inner; // E rules: the oracle rule that matched after decoding
    bool operator<(const Hit &o) const;
    bool operator==(const Hit &o) const { return line == o.line && rule == o.rule && description == o.description; }
};
constexpr int MaxHexWords = 4, AsmRun = 3, AsmBlock = 6, MaxJsonFailures = 2000, MaxJsonScan = 5000000, MaxLineLength = 4096;
// Oracle rules R1-R8, sorted and de-duplicated like check_text().
QList<Hit> checkText(const QString &text);
// checkText() with R7 (the JSON scan) optional.
QList<Hit> checkTextImpl(const QString &text, bool scanJson);
// checkText() plus launcher-only rules on decoded variants of the text: E1 look-alike letters folded,
// E2 base64 blobs, E3 \x \u % &# escapes; E4 flags lines over MaxLineLength and re-checks them in short
// windows (a window hit carries its rule as inner); E5 re-checks text with NUL/U+FFFD removed. Stricter than the oracle.
QList<Hit> checkTextStrict(const QString &text);
// R1, R3, R5, R10 (and E hits of those) suggest exposure to disassembly: report and taint, not revise.
bool isComplianceRule(const Hit &hit);
// Narrow scan for agent logs and notes (which may quote Darling's own assembly or C): tool-dump
// signatures (R3) and decompiler names/byte macros (part of R5) only.
QList<Hit> exposureMarkers(const QString &text);
QList<Hit> rawTraceObjects(const QString &text); // R7 alone, for scanning workspaces
// F1: a recon->impl file is exactly the three fixed statements (plus Agent:/Inputs: headers).
QList<Hit> reconFormat(const QString &text);
std::optional<QString> parseHeader(const QString &text, const QString &label);
QStringList parseList(const QString &text, const QString &label);
QStringList parseSources(const QString &text);
}
