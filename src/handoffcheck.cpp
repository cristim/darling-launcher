// SPDX-License-Identifier: GPL-3.0-or-later
#include "handoffcheck.h"
#include <QByteArray>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <algorithm>
#include <cctype>
#include <functional>
// Rule tables mirror check_handoff.py one to one; the parity test compares both on tests/handoff-corpus.
namespace {
using RX = QRegularExpression;
// Python's \w is letters, numbers and '_'; PCRE2 >= 10.43 with UCP also counts combining marks (Mn)
// and connector punctuation (Pc), which would hide a boundary Python sees. Spell \w and \b out.
QString pythonWordClasses(const QString &pattern) {
    const QString word = R"(\p{L}\p{N}_)", boundary = "(?:(?<=[" + word + "])(?![" + word + "])|(?<![" + word + "])(?=[" + word + "]))";
    QString out; bool inClass = false;
    for (qsizetype i = 0; i < pattern.size(); ++i) {
        const QChar c = pattern[i];
        if (c == '\\' && i + 1 < pattern.size()) {
            const QChar e = pattern[++i];
            if (e == 'w') out += inClass ? word : "[" + word + "]";
            else if (e == 'b' && !inClass) out += boundary;
            else out += QString(c) + e;
            continue;
        }
        if (c == '[' && !inClass) inClass = true;
        else if (c == ']' && inClass && !out.endsWith('[') && !out.endsWith("[^")) inClass = false;
        out += c;
    }
    return out;
}
RX rx(const QString &pattern, bool caseless = false) {
    // (*LF): only \n ends a line, like Python's '.' and '$'. UCP: Unicode \d \s and case folding, like Python str patterns.
    return RX("(*LF)" + pythonWordClasses(pattern), RX::UseUnicodePropertiesOption | (caseless ? RX::CaseInsensitiveOption : RX::NoPatternOption));
}
const QString H = "[0-9a-fA-F]";
const QString REG = R"(%?(?:[xw](?:[0-9]|[12][0-9]|30)|sp|lr|fp|xzr|wzr|[qdsvb][0-9]{1,2}(?:\.\w+)?|[re]?(?:ax|bx|cx|dx|si|di|bp|sp|ip)|[abcd][lh]|r(?:[89]|1[0-5])[dwb]?|xmm[0-9]+))";
const QString IMM = R"((?:[#$]-?(?:0x[0-9a-f]+|[0-9]+)|-?0x[0-9a-f]+|-?[0-9]+))";
const QString MEM = R"((?:(?:[a-z]+\s+ptr\s+)?(?:[a-z]s:)?\[[^\]\n]{1,40}\]!?|-?(?:0x[0-9a-f]+|[0-9]+)?\(%[a-z0-9]+[^)\n]{0,20}\)))";
const QString LABEL = R"(\*?[A-Za-z_.$][\w.$@]*)";
const QString OPS = "(?:" + REG + "|" + IMM + "|" + MEM + "|" + LABEL + ")";
QString mnemonics() {
    QStringList all = QString("mov movz movk movn ldr ldp ldur ldrb ldrh ldrsw str stp stur strb strh add adds sub subs "
        "adrp adr cmp cmn tst cbz cbnz tbz tbnz bl blr br csel cset orr and eor lsl lsr asr svc "
        "mul madd ubfx sbfx ldar ldaxr stlr stlxr fmov casal cas ldxr stxr "
        "mov movq movl movzx movsx push pop call lea jmp je jne jz jnz ja jb jg jl jle jge "
        "cmp test xor add sub and or imul shl shr inc dec").split(' ', Qt::SkipEmptyParts);
    all.removeDuplicates();
    std::stable_sort(all.begin(), all.end(), [](const QString &a, const QString &b) { return a.size() > b.size(); });
    return all.join('|');
}
const QSet<QString> BRANCH{"bl", "blr", "br", "call", "jmp", "je", "jne", "jz", "jnz", "ja", "jb", "jg", "jl", "jle", "jge", "cbz", "cbnz"};
const RX ASM_LINE = rx(R"(^\s*(?:(?:0x)?[0-9a-f]{4,16}:\s*)?(?<m>)" + mnemonics() + R"(|b\.[a-z]{2})(?<ops>\s+)" + OPS + R"((?:\s*,\s*)" + OPS + R"(){0,3})\s*(?:(?:;|//|@).*)?$)", true);
const RX BARE_LINE = rx(R"(^\s*(?:(?:0x)?[0-9a-f]{4,16}:\s*)?(?:ret|retq|leave|nop|cltq|cqto|hlt|int3)\s*(?:(?:;|//).*)?$)", true);
const RX STRICT_OP = rx("^(?:" + REG + "|" + MEM + R"(|[#$]-?(?:0x[0-9a-f]+|[0-9]+)|-?0x[0-9a-f]+)$)", true);
const RX DECIMAL = rx(R"(^-?[0-9]+\z)");
struct LineRule { QString id, description; RX pattern; };
const QList<LineRule> LINE_RULES{
    {"R2", "address+offset notation", rx(R"(<\+\d+>|<[\w.$]+\+(?:0x)" + H + R"(+|\d+)>|^\s*(?:=>\s*)?0x)" + H + R"({8,16}\s*:|\b[A-Za-z_]\w*\s*\+\s*0x)" + H + R"(+\b|\b_[A-Za-z]\w*\+\d+\b)", true)},
    {"R3", "disassembler/dump tool output signature", rx(R"(Contents of \(__\w+,__\w+\)|\(__[A-Z]+,__\w+\)\s+section|file format\s+(?:mach-?o|elf\d*)|^\s*cmd\s+LC_\w+|^\s*Idx\s+Name\s+Size\s+VMA|^\s*(?:rebase|lazy bind|weak bind|bind|export) information)", true)},
    {"R4", "hex-dump line", rx(R"(^\s*(?:0x)?)" + H + R"({4,16}:?\s+(?:)" + H + R"({2}\s+){8,}|^\s*)" + H + R"({6,16}:\s+(?:)" + H + R"({4}\s+){4,})")},
    {"R5", "decompiler pseudocode marker", rx(R"(\b(?:sub|loc)_[0-9A-Fa-f]+\b|\b(?:LOBYTE|HIBYTE|LOWORD|HIWORD|LODWORD|HIDWORD)\s*\(|(?<![\w.])v\d+\s*=(?!=)|\b__int(?:8|16|32|64)\b|\b_(?:QWORD|DWORD|WORD|BYTE)\b|\b__(?:fastcall|cdecl|stdcall|usercall)\b)")},
    {"R6", "path into Apple system image, install volume or shared cache", rx(R"(/System/(?:Library|Volumes|Cryptexes|Applications)|/Library/Apple\b|/Volumes/[^/\s]+/(?:System|Library|usr|Applications)\b|dyld_shared_cache|\.dyld\b|/usr/lib/system/\S*\.dylib)", true)},
    {"R8", "forbidden trace field as key", rx(R"(["'](?:lr|pc|backtrace|image_offset|return_address)["']\s*:|(?<![\w.])(?:lr|pc)\s*[:=]\s*0x[0-9a-f]+|(?<![\w.])(?:backtrace|image_offset|return_address)\s*[:=]\s*(?:0x[0-9a-f]+|\[|\{|\d))", true)},
};
// R10 (ruling F3): an address or offset inside an Apple library image.
const QString APPLE_LIB = R"((?:\blib[\w+-]*(?:\.[\w.]+)?|\b\w+\.framework\b|\b\w+\.dylib\b|\b(?:AppKit|Foundation|CoreFoundation|CoreGraphics|CoreServices|Security|QuartzCore|IOKit|SystemConfiguration|CFNetwork|CoreText|CoreAudio|AVFoundation|Metal|ImageIO|LaunchServices|ApplicationServices|UIKit|WebKit|Carbon|DiskArbitration|CoreData)\b))";
const RX LIB_OFFSET = rx(APPLE_LIB + R"([^\n]{0,30}?(?:[+:@]\s*|\boffset\s+|\bat\s+)0x)" + H + "+", true);
const RX LIB_ONLY = rx(APPLE_LIB, true);
const RX PC_VALUE = rx(R"(\b(?:pc|lr|rip|return\s+addr(?:ess)?|frame(?:\s+addr(?:ess)?)?)\b[^\n]{0,20}?0x)" + H + "{4,}", true);
const RX CLASSDUMP_IVAR = rx(R"(;\s*(?://|/\*)\s*(?:\d+\s*=\s*)?[+@]?0x)" + H + "+");
const RX OBJC_INTERFACE = rx(R"(^\s*@interface\s+\w+)");
const RX HEX_WORD = rx(R"((?<![\w])(?:0[xX][0-9a-fA-F_']{8,24}|(?:)" + H + "{4}_){1,3}" + H + "{4}|" + H + R"({16}|[0-9]{15,20}|(?=[0-9a-fA-F]*[0-9])(?=[0-9a-fA-F]*[a-fA-F])[0-9a-fA-F]{8,15})(?![\w]))");
const RX MD_PREFIX = rx(R"(^[\s>|]*(?:(?:[-*+\x{2022}]|\d+[.)])\s+)*)");
const RX TRACE_REG_KEY = rx(R"(^(?:x[0-7]|rdi|rsi|rdx|rcx|r8|r9)$)");

// Python str.isspace() also covers U+001C..U+001F, which QChar::isSpace() and PCRE's \s do not.
bool pySpace(QChar c) { return c.isSpace() || (c.unicode() >= 0x1c && c.unicode() <= 0x1f); }
QString pyStrip(const QString &s, const std::function<bool(QChar)> &drop = pySpace) {
    qsizetype b = 0, e = s.size();
    while (b < e && drop(s[b])) ++b;
    while (e > b && drop(s[e - 1])) --e;
    return s.mid(b, e - b);
}
QString stripMd(const QString &line) {
    QString s = line; const auto m = MD_PREFIX.match(s);
    if (m.hasMatch()) s.remove(0, m.capturedLength());
    s = pyStrip(s); s = pyStrip(s, [](QChar c) { return c == '`' || c == '|'; });
    return pyStrip(s);
}
bool asmLike(const QString &line) {
    if (BARE_LINE.match(line).hasMatch()) return true;
    const auto m = ASM_LINE.match(line);
    if (!m.hasMatch()) return false;
    if (BRANCH.contains(m.captured("m").toLower())) return true;
    // Without a register, memory operand or prefixed immediate, "add 5" is prose.
    for (const QString &op : m.captured("ops").split(',')) {
        const QString o = pyStrip(op);
        if (STRICT_OP.match(o).hasMatch() && !DECIMAL.match(o).hasMatch()) return true;
    }
    return false;
}

// Mirrors json.JSONDecoder.raw_decode: one value from a position, Python's grammar (NaN/Infinity,
// strict strings, last duplicate key wins), reporting whether any object in it looks like a raw
// trace record (R7).
struct JsonScan {
    const QString &s; qsizetype i = 0;
    QChar at() const { return i < s.size() ? s[i] : QChar(); }
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; }
    bool lit(const char *w) { const QString word = QString::fromLatin1(w); if (s.mid(i, word.size()) != word) return false; i += word.size(); return true; }
    bool string(QString *out) {
        if (at() != '"') return false;
        ++i;
        while (i < s.size()) {
            const QChar c = s[i++];
            if (c == '"') return true;
            if (c.unicode() < 0x20) return false;
            if (c != '\\') { if (out) *out += c; continue; }
            if (i >= s.size()) return false;
            const QChar e = s[i++]; static const QHash<QChar, QChar> simple{{'"', '"'}, {'\\', '\\'}, {'/', '/'}, {'b', '\b'}, {'f', '\f'}, {'n', '\n'}, {'r', '\r'}, {'t', '\t'}};
            if (simple.contains(e)) { if (out) *out += simple[e]; continue; }
            const QString hex = s.mid(i, 4);
            if (e != 'u' || hex.size() != 4 || !std::all_of(hex.begin(), hex.end(), [](QChar h) { return h.unicode() < 128 && isxdigit(h.unicode()); })) return false;
            i += 4; if (out) *out += QChar(hex.toUShort(nullptr, 16));
        }
        return false;
    }
    bool number() {
        const qsizetype start = i;
        if (at() == '-') ++i;
        if (lit("Infinity")) return true;
        auto digits = [&] { const qsizetype from = i; while (at().unicode() >= '0' && at().unicode() <= '9') ++i; return i - from; };
        if (at() == '0') ++i;
        else if (at().unicode() < '1' || at().unicode() > '9' || !digits()) { i = start; return false; }
        const qsizetype intDigits = i - start - (s[start] == '-');
        bool fraction = false;
        if (at() == '.') { const qsizetype dot = i++; if (digits()) fraction = true; else i = dot; }
        if (at() == 'e' || at() == 'E') { const qsizetype e = i++; if (at() == '+' || at() == '-') ++i; if (digits()) fraction = true; else i = e; }
        return fraction || intDigits <= 4300; // Python refuses int() of more than 4300 digits
    }
    bool value(int depth, bool &trace) {
        trace = false;
        if (depth > 1000) return false; // Python raises RecursionError somewhere near here; both count a failure
        const QChar c = at();
        if (c == '{') {
            ++i; ws(); QHash<QString, bool> children; QSet<QString> keys;
            if (at() == '}') { ++i; return true; }
            for (;;) {
                QString key; bool nested = false;
                if (!string(&key)) return false;
                keys.insert(key.toLower()); ws();
                if (at() != ':') return false;
                ++i; ws();
                if (!value(depth + 1, nested)) return false;
                children.insert(key, nested); ws();
                if (at() == '}') { ++i; break; }
                if (at() != ',') return false;
                ++i; ws();
            }
            for (bool nested : children) trace = trace || nested;
            static const QSet<QString> symbolKeys{"symbol", "sym", "name"}, strongArgs{"regs", "arg_regs", "fp_regs", "words"};
            if (keys.intersects(symbolKeys)) {
                trace = trace || keys.intersects(strongArgs) || (keys.contains("args") && (keys.contains("symbol") || keys.contains("sym")));
                for (const QString &k : keys) trace = trace || TRACE_REG_KEY.match(k).hasMatch();
            }
            return true;
        }
        if (c == '[') {
            ++i; ws();
            if (at() == ']') { ++i; return true; }
            for (;;) {
                bool nested = false;
                if (!value(depth + 1, nested)) return false;
                trace = trace || nested; ws();
                if (at() == ']') { ++i; return true; }
                if (at() != ',') return false;
                ++i; ws();
            }
        }
        if (c == '"') return string(nullptr);
        return lit("true") || lit("false") || lit("null") || lit("NaN") || number();
    }
};
QList<int> traceHits(const QString &text) {
    QList<int> hits; int fails = 0, line = 1;
    qsizetype i = text.indexOf('{'), counted = 0;
    while (i != -1) {
        JsonScan scan{text, i}; bool trace = false;
        if (!scan.value(0, trace)) {
            if (++fails > HandoffCheck::MaxJsonFailures) return hits << 0;
            i = text.indexOf('{', i + 1); continue;
        }
        if (trace) { line += int(QStringView(text).mid(counted, i - counted).count('\n')); counted = i; hits << line; }
        i = text.indexOf('{', scan.i);
    }
    return hits;
}
QList<HandoffCheck::Hit> sortedUnique(QList<HandoffCheck::Hit> hits) {
    std::sort(hits.begin(), hits.end());
    hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
    return hits;
}
QString normalize(const QString &text) {
    const QString nfkc = text.normalized(QString::NormalizationForm_KC);
    // Format characters (zero-width joiners, BOM, soft hyphen) split tokens invisibly; lone CR, NEL, LS, PS are line breaks.
    QString t; t.reserve(nfkc.size());
    for (qsizetype i = 0; i < nfkc.size(); ++i) {
        const char32_t c = nfkc[i].isHighSurrogate() && i + 1 < nfkc.size() ? QChar::surrogateToUcs4(nfkc[i], nfkc[i + 1]) : nfkc[i].unicode();
        const int width = c > 0xffff ? 2 : 1;
        if (QChar::category(c) != QChar::Other_Format) t += nfkc.mid(i, width);
        i += width - 1;
    }
    t.replace("\r\n", "\n");
    for (const QChar br : {QChar('\r'), QChar(0x85), QChar(0x2028), QChar(0x2029)}) t.replace(br, QChar('\n'));
    return t;
}
// Python's re.I folds U+0130/U+0131 with i/I; PCRE2 deliberately does not. Same length, so positions hold.
QString caselessView(const QString &line) {
    QString out = line;
    for (QChar &c : out) { if (c.unicode() == 0x130) c = 'I'; else if (c.unicode() == 0x131) c = 'i'; }
    return out;
}
// A long line as overlapping pieces short enough to match reliably; the overlap exceeds any marker.
QStringList windows(const QString &line) {
    constexpr int Overlap = 512;
    if (line.size() <= HandoffCheck::MaxLineLength) return {line};
    QStringList out;
    for (qsizetype at = 0; at < line.size(); at += HandoffCheck::MaxLineLength - Overlap) {
        out << line.mid(at, HandoffCheck::MaxLineLength);
        if (at + HandoffCheck::MaxLineLength >= line.size()) break;
    }
    return out;
}
bool search(const RX &pattern, const QString &line, const QString &caseless) {
    return pattern.match(pattern.patternOptions() & RX::CaseInsensitiveOption ? caseless : line).hasMatch();
}
}
bool HandoffCheck::Hit::operator<(const Hit &o) const {
    if (line != o.line) return line < o.line;
    if (rule != o.rule) return rule < o.rule;
    return description < o.description;
}
QList<HandoffCheck::Hit> HandoffCheck::checkText(const QString &input) {
    const QString text = normalize(input);
    QStringList lines;
    for (QString l : text.split('\n')) {
        // Python's \s and strip() treat U+001C..U+001F as whitespace; PCRE does not.
        for (QChar &c : l) if (c.unicode() >= 0x1c && c.unicode() <= 0x1f) c = ' ';
        lines << stripMd(l);
    }
    QList<Hit> hits;
    QStringList ci;
    for (const QString &l : lines) ci << caselessView(l);
    const bool hasInterface = std::any_of(lines.begin(), lines.end(), [](const QString &l) { return OBJC_INTERFACE.match(l).hasMatch(); });
    for (int i = 0; i < lines.size(); ++i) {
        for (const auto &rule : LINE_RULES) if (search(rule.pattern, lines[i], ci[i])) hits << Hit{i + 1, rule.id, rule.description};
        if (hasInterface && (OBJC_INTERFACE.match(lines[i]).hasMatch() || CLASSDUMP_IVAR.match(lines[i]).hasMatch())) hits << Hit{i + 1, "R3", "class-dump style @interface with ivar offsets"};
    }
    for (int i = 0; i < lines.size(); ++i) {
        const auto nearLibrary = [&] { for (int k = qMax(0, i - 1); k <= qMin(int(lines.size()) - 1, i + 1); ++k) if (LIB_ONLY.match(ci[k]).hasMatch()) return true; return false; };
        if (LIB_OFFSET.match(ci[i]).hasMatch() || (PC_VALUE.match(ci[i]).hasMatch() && nearLibrary()))
            hits << Hit{i + 1, "R10", "address/offset inside an Apple library image (library+offset, pc/lr/frame value)"};
    }
    for (int line : traceHits(text)) hits << Hit{line, "R7", line ? "raw trace (T0) JSON object" : "too many JSON candidates to scan"};

    QList<int> block, run;
    const QStringList r1Lines = lines + QStringList{""};
    for (int i = 1; i <= r1Lines.size(); ++i) {
        const QString &line = r1Lines[i - 1];
        if (pyStrip(line).isEmpty()) {
            if (block.size() >= AsmBlock) hits << Hit{block[0], "R1", QString::number(block.size()) + " instruction-like lines in one block"};
            block.clear(); run.clear(); continue;
        }
        if (asmLike(caselessView(line))) {
            block << i; run << i;
            if (run.size() == AsmRun) hits << Hit{run[0], "R1", QString::number(AsmRun) + "+ consecutive instruction-like lines"};
        } else run.clear();
    }

    // R4: consecutive hex words across adjacent lines; blank lines do not reset the run, prose does.
    long long total = 0; int start = 0;
    const QStringList r4Lines = lines + QStringList{"x"};
    for (int i = 1; i <= r4Lines.size(); ++i) {
        int n = 0;
        for (auto it = HEX_WORD.globalMatch(r4Lines[i - 1]); it.hasNext();) {
            QString word = it.next().captured();
            if (word.startsWith("0x")) word.remove(0, 2);
            if (word.startsWith("0X")) word.remove(0, 2);
            word.remove('_'); word.remove('\'');
            n += word.size() >= 8;
        }
        if (n) {
            start = start ? start : i; total += n;
            if (total > MaxHexWords) { hits << Hit{start, "R4", "more than " + QString::number(MaxHexWords) + " consecutive 32/64-bit hex or decimal words"}; total = -1000000000LL; }
        } else if (!pyStrip(r4Lines[i - 1]).isEmpty()) { total = 0; start = 0; }
    }
    return sortedUnique(hits);
}
namespace {
QString foldLookalikes(const QString &text) {
    static const QHash<QChar, QChar> map = [] {
        QHash<QChar, QChar> m;
        const QString from = QString::fromUtf8("аеорсухіјѕԁһӏԛԝгкмптвАВЕКМНОРСТХУІЈЅαονρτυικχεΑΒΕΖΗΙΚΜΝΟΡΤΥΧıɑɡ");
        const QString to = QStringLiteral("aeopcyxijsdhlqwrkmntbABEKMHOPCTXYIJSaovptuikxeABEZHIKMNOPTYXiag");
        for (qsizetype i = 0; i < from.size(); ++i) m.insert(from[i], to[i]);
        return m;
    }();
    QString out = text;
    for (QChar &c : out) c = map.value(c, c);
    return out;
}
QString decodeEscapes(const QString &text) {
    static const RX escape(R"(\\x([0-9a-fA-F]{2})|\\u([0-9a-fA-F]{4})|%([0-9a-fA-F]{2})|&#([0-9]{1,6});|&#[xX]([0-9a-fA-F]{1,6});)");
    QString out; qsizetype last = 0;
    for (auto it = escape.globalMatch(text); it.hasNext();) {
        const auto m = it.next(); out += text.mid(last, m.capturedStart() - last); last = m.capturedEnd();
        const int base = m.capturedLength(4) ? 10 : 16; uint code = 0;
        for (int g = 1; g <= 5; ++g) if (m.capturedLength(g)) code = m.captured(g).toUInt(nullptr, base);
        out += code && code <= 0x10ffff ? QString::fromUcs4(reinterpret_cast<const char32_t *>(&code), 1) : m.captured();
    }
    return out + text.mid(last);
}
// Base64 blobs that decode to mostly printable UTF-8 text; a sha256 in hex decodes to binary and is skipped.
QList<QPair<int, QString>> base64Texts(const QString &text) {
    static const RX blob(R"((?<![A-Za-z0-9+/=])[A-Za-z0-9+/]{16,}={0,2}(?![A-Za-z0-9+/=]))");
    QList<QPair<int, QString>> out;
    int line = 1; qsizetype counted = 0; // line numbers in one pass, not a recount per blob
    for (auto it = blob.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        line += int(QStringView(text).mid(counted, m.capturedStart() - counted).count('\n')); counted = m.capturedStart();
        const auto decoded = QByteArray::fromBase64Encoding(m.captured().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded || decoded.decoded.size() < 8) continue;
        QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
        const QString s = utf8.decode(decoded.decoded);
        if (utf8.hasError()) continue;
        const auto printable = std::count_if(s.begin(), s.end(), [](QChar c) { return c.isPrint() || c == '\n' || c == '\t' || c == '\r'; });
        if (printable * 10 >= s.size() * 9) out << qMakePair(line, s);
    }
    return out;
}
}
QList<HandoffCheck::Hit> HandoffCheck::checkTextStrict(const QString &input) {
    QList<Hit> hits = checkText(input);
    const QList<Hit> base = hits;
    auto addNew = [&](const QString &id, const QString &what, const QList<Hit> &found, int fixedLine) {
        for (const Hit &h : found) {
            if (fixedLine == 0 && base.contains(h)) continue;
            hits << Hit{fixedLine ? fixedLine : h.line, id, what + " matches " + h.rule + " (" + h.description + ")", h.rule};
        }
    };
    const QString text = normalize(input);
    if (const QString folded = foldLookalikes(text); folded != text) addNew("E1", "text with look-alike letters folded to Latin", checkText(folded), 0);
    if (const QString decoded = decodeEscapes(text); decoded != text) addNew("E3", "text with escapes decoded", checkText(decoded), 0);
    // NUL and undecodable bytes (U+FFFD after decoding) are rejected as R9 by the store, but must not split a
    // compliance marker into a mere revise.
    if (QString cleaned = text; cleaned.remove(QChar(0)).remove(QChar(0xfffd)) != text) addNew("E5", "text with NUL or undecodable bytes removed", checkText(cleaned), 0);
    for (const auto &[line, decoded] : base64Texts(text)) addNew("E2", "base64-decoded blob", checkText(decoded), line);
    // PCRE reports a hit-limit error as "no match"; very long lines would otherwise pass unchecked.
    const QStringList lines = text.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        if (lines[i].size() <= MaxLineLength) continue;
        hits << Hit{i + 1, "E4", "line longer than " + QString::number(MaxLineLength) + " characters"};
        // Re-check the long line in short windows, so a match-limit error can never hide a compliance hit.
        QSet<QString> seen;
        for (const QString &window : windows(lines[i]))
            for (const Hit &h : checkText(window))
                if (!seen.contains(h.rule)) { seen.insert(h.rule); hits << Hit{i + 1, "E4", "long line, a window matches " + h.rule + " (" + h.description + ")", h.rule}; }
    }
    return sortedUnique(hits);
}
QList<HandoffCheck::Hit> HandoffCheck::exposureMarkers(const QString &input) {
    static const RX markers = rx(R"(\b(?:sub|loc)_[0-9A-Fa-f]+\b|\b(?:LOBYTE|HIBYTE|LOWORD|HIWORD|LODWORD|HIDWORD)\s*\()");
    const QStringList lines = foldLookalikes(normalize(input)).split('\n');
    QList<Hit> hits;
    for (int i = 0; i < lines.size(); ++i)
        for (const QString &window : windows(stripMd(lines[i]))) {
            if (search(LINE_RULES[1].pattern, window, caselessView(window))) hits << Hit{i + 1, "R3", LINE_RULES[1].description};
            if (markers.match(window).hasMatch()) hits << Hit{i + 1, "R5", "decompiler name or byte-access marker"};
        }
    return sortedUnique(hits);
}
QList<HandoffCheck::Hit> HandoffCheck::rawTraceObjects(const QString &text) {
    QList<Hit> hits;
    for (int line : traceHits(normalize(text))) hits << Hit{line, "R7", line ? "raw trace (T0) JSON object" : "too many JSON candidates to scan"};
    return hits;
}
bool HandoffCheck::isComplianceRule(const Hit &hit) {
    static const QSet<QString> rules{"R1", "R3", "R5", "R10"};
    return rules.contains(hit.rule) || rules.contains(hit.inner);
}
QList<HandoffCheck::Hit> HandoffCheck::reconFormat(const QString &text) {
    static const QString name = R"([A-Za-z_$][\w$.@]*)";
    static const RX header = rx(R"(^\s*(?:Agent|Inputs):)", true);
    static const RX r6 = LINE_RULES[4].pattern;
    static const RX path = rx(R"(^[\w.+-]+(?:/[\w.+-]+)*\z)");
    static const QList<QPair<RX, QString>> expected{
        {rx("^Absent:\\s*(" + name + "(?:\\s*,\\s*" + name + ")*)\\z"), "Absent: <names>"},
        {rx(R"(^Branch:\s*[\w][\w./-]*\s+Commit:\s*[0-9a-fA-F]{40}\z)"), "Branch: <name>  Commit: <40-hex SHA>"},
        {rx(R"(^Callers exist:\s*(yes|no)(?:\s*;\s*files:\s*(.+))?\z)"), "Callers exist: yes|no [; files: <paths>]"}};
    QList<QPair<int, QString>> body;
    const QStringList lines = text.split('\n');
    for (int i = 0; i < lines.size(); ++i) if (!pyStrip(lines[i]).isEmpty() && !header.match(lines[i]).hasMatch()) body << qMakePair(i + 1, pyStrip(lines[i]));
    QList<Hit> out;
    for (int k = 0; k < expected.size(); ++k) {
        if (k >= body.size()) { out << Hit{body.isEmpty() ? 0 : body.last().first, "F1", QString("missing line %1: expected '%2'").arg(k + 1).arg(expected[k].second)}; continue; }
        const auto &[ln, line] = body[k];
        const auto m = expected[k].first.match(line);
        if (!m.hasMatch()) { out << Hit{ln, "F1", QString("line %1 does not match '%2'").arg(k + 1).arg(expected[k].second)}; continue; }
        if (k != 2) continue;
        const QString files = m.captured(2);
        if (m.captured(1) == "yes" && files.isEmpty()) out << Hit{ln, "F1", "'Callers exist: yes' needs '; files: <paths>'"};
        else if (m.captured(1) == "no" && !files.isEmpty()) out << Hit{ln, "F1", "'Callers exist: no' must not list files"};
        for (const QString &raw : files.isEmpty() ? QStringList() : files.split(',')) {
            const QString p = pyStrip(raw);
            if (!path.match(p).hasMatch() || p.startsWith('/') || p.split('/').contains("..") || r6.match(p).hasMatch()) {
                out << Hit{ln, "F1", "file path must be repo-relative (no leading '/', no '..', no system paths)"}; break;
            }
        }
    }
    if (body.size() > 3) out << Hit{body[3].first, "F1", "more than 3 non-empty content lines"};
    return out;
}
std::optional<QString> HandoffCheck::parseHeader(const QString &text, const QString &label) {
    const RX header = rx(R"(^\s*(?:[*_-]\s*)?\**)" + label + R"(:\**\s*(.*)$)", true);
    for (const QString &line : text.split('\n')) {
        const auto m = header.match(caselessView(line)); // same length as line, so offsets apply to it
        if (m.hasMatch()) return pyStrip(line.mid(m.capturedStart(1), m.capturedLength(1)));
    }
    return std::nullopt;
}
QStringList HandoffCheck::parseList(const QString &text, const QString &label) {
    QStringList out;
    for (const QString &part : parseHeader(text, label).value_or(QString()).split(';')) if (!pyStrip(part).isEmpty()) out << pyStrip(part);
    return out;
}
QStringList HandoffCheck::parseSources(const QString &text) { return parseList(text, "Sources?"); }
