#!/usr/bin/env python3
"""Regenerate tests/handoff-corpus/ (run: python3 tests/handoff-corpus/generate.py).

Rows: (1) every text the CURRENT oracle test suite (tests/handoff-oracle/test_check_handoff.py) passes to
check_text() or recon_format(), captured by running that suite: oracle-<sha>.txt and recon-<sha>.txt;
(2) the named rows below (earlier fixtures, json/word-boundary parity cases, adversarial encodings).
expected.tsv holds the ORACLE's own verdict (check_text, plus recon_format for recon-* rows) and the
launcher-only E rules expected on top."""
import base64, contextlib, hashlib, io, os, sys, tempfile, unittest
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "handoff-oracle"))
import check_handoff as ch

out = sys.argv[1] if len(sys.argv) > 1 else HERE
os.makedirs(out, exist_ok=True)
N = ch.MAX_HEX_WORDS
cases = []
def add(name, text, extra=""):
    cases.append((name, text, extra))

# --- fixtures from test_check_handoff.py RuleTests (same strings) ---
add("r1-run", "mov x0, x1\nldr x2, [sp, #8]\nbl _foo\n")
add("r1-x86-block", "push rbp\nnote\nmov rbp, rsp\nnote\nlea rax, [rbp-8]\nnote\ncall 0x10\nnote\npop rbp\nnote\nret\n")
add("r1-near-prose", "We mov the file, then call the function and push the button.\n")
add("r1-near-two", "mov x0, x1\nldr x2, [sp, #8]\n")
add("r1-near-noops", "add 5\nsub 3\nand then\ncall foo bar\n")
for i, t in enumerate(("see <+28> here", "0x100003f40: foo", "at main+0x1c", "<_main+28>")):
    add(f"r2-{i}", t)
add("r2-near", "version 1+2 is fine; 0x10 is a flag; main + 1\n")
for i, t in enumerate(("Contents of (__TEXT,__text) section", "foo: file format mach-o arm64", "Idx Name Size VMA", "    cmd LC_SEGMENT_64")):
    add(f"r3-{i}", t)
add("r3-classdump", "@interface Foo : NSObject\n{\n    int _a;  // 8 = 0x8\n}\n@end\n")
add("r3-near-interface", "The @interface keyword declares a class.\n")
add("r3-near-ivar", "int _a;  // 8 = 0x8\n")
add("r3-near-otool-L", "otool -L output: /usr/lib/libSystem.B.dylib (compatibility version 1.0.0)\n")
add("r4-hexdump-bytes", "00001000  de ad be ef 00 11 22 33  44\n")
add("r4-hexdump-words", "00000010: 4865 6c6c 6f20 776f 726c\n")
add("r4-run-lines", "\n".join(f"0x00007ffe0000000{i}" for i in range(N + 1)))
add("r4-run-oneline", "0x00007ffe00000001 0x00007ffe00000002 0x00007ffe00000003 0x00007ffe00000004 0x00007ffe00000005")
add("r4-near-four", "\n".join(f"0x00007ffe0000000{i}" for i in range(N)))
add("r4-near-sep", "0x00007ffe00000001\ntext\n0x00007ffe00000002\n" * 3)
add("r4-near-sha", "sha " + "a" * 64 + "\nflag 0xff 0x10\n")
for i, t in enumerate(("calls sub_1A2B", "goto loc_401000", "x = LODWORD(y)", "v12 = a1", "__int64 f", "_QWORD *p")):
    add(f"r5-{i}", t)
for i, t in enumerate(("calls FUN_00101a2b", "undefined4 uVar1;\nuVar1 = param_1 + 4;", "undefined8 iVar1;\n*(int *)(local_28 + 8) = 0;", "x = PTR_FUN_00401000;",
                       "x = _DAT_100008000;", "y = CONCAT44(a, b);", "undefined8 foo(void)\nlocal_28 = 0;", "puVar3 = LAB_00101000;",
                       "undefined8 pppuVar1 = in_RDI;", "x = in_stack_ffffffe8; y = unaff_RBX; z = extraout_RAX;", "y = SUB84(a, 0); w = ZEXT416(a);")):
    add(f"r5-ghidra-{i}", t)
for i, t in enumerate(("envVar1", "newVar2", "param_1", "local_cafe", "DAT_1234", "undefined2", "in_RDI")):
    add(f"r5-near-ghidra-weak-{i}", f"see {t} here\n")
add("r5-near-ghidra-weak-mix", "def f(param_1):\n    local_1 = param_1\n    iVar1 = local_1\nThe spec names param_1 and envVar1.\n")
add("r5-near-ghidra-blocks", "undefined4 x;\n\nuVar1 = param_1 + 4;\n")
add("r1-near-arm32-words", "bne tomorrow\nbeq today\nbge soon\nblt later\n")
add("r1-armv7-imm", "mov r0, #1\nmov r1, #2\ncmp r0, #0\nmovl $1, 4\nb #0x100\n")
add("r5-near-ghidra", "The function is undefined, the local variable param is unnamed; a data file DAT holds it.\nlocal_name and param_x and funny_ones\n")
add("r5-near", "the sub_system loc is vague; v == 3; version = 2\n")
for i, t in enumerate(("/System/Library/Frameworks/Foo.framework/Foo", "/Volumes/MacintoshHD/System/x",
                       "dyld_shared_cache_arm64e", "libfoo.dyld", "/usr/lib/system/libsystem_c.dylib", "/Library/Apple/usr/bin")):
    add(f"r6-{i}", t)
add("r6-near", "Foo.framework, libSystem.B.dylib, /usr/lib/libSystem.B.dylib, ~/prefix/Library\n")
add("r7-trace", '{"symbol": "_foo", "lib": "libx", "regs": [1, 2]}\n')
add("r7-sym-x0", '{"sym": "_f", "x0": 5}')
add("r7-near-title", '{"title": "notes", "count": 3}\n')
add("r7-near-symbol-only", '{"symbol": "_foo"}\n')
add("r7-near-name-args", '{"name": "x", "args": [1]}\n')
add("r7-near-braces", "braces { like } this and {not json}\n")
for i, c in enumerate(['{\n  "symbol": "_f",\n  "regs": [1,\n 2]\n}\n', '- {"symbol": "_f", "x0": 1}\n',
                       '1. `{"sym": "_f", "regs": []}`\n', '| a | {"symbol": "_f", "words": [1]} |\n',
                       '[{"ok": 1}, {"symbol": "_f", "arg_regs": []}]\n',
                       '{"outer": {"inner": [{"symbol": "_f", "regs": []}]}}\n',
                       'text {"name": "_f", "regs": [1]} text\n', '﻿{"symbol": "_f", "regs": []}\n']):
    add(f"r7-shape-{i}", c)
add("r7-brace-flood", '{"a":' * (ch.MAX_JSON_FAILURES + 5), "E4")
for k in ("lr", "pc", "backtrace", "image_offset", "return_address"):
    add(f"r8-key-{k}", f'{{"{k}": 1}}')
for i, t in enumerate(('{"a": [{"b": {"LR": 1}}]}', "lr: 0x1c", "PC = 0x10", "backtrace: [", '"Return_Address": 5')):
    add(f"r8-form-{i}", t)
add("r8-near", "The pc and lr registers are not recorded; backtrace is forbidden.\n")
for i, pre in enumerate(("- ", "* ", "1. ", "2) ", "> ", "`")):
    suf = "`" if pre == "`" else ""
    add(f"md-prefix-{i}", "\n".join(f"{pre}{l}{suf}" for l in ("MOV X0, X1", "ldr x2, [sp, #8]", "bl _foo")))
add("unicode-nbsp", "mov x0, x1\nldr x2, [sp, #8]\nbl _foo")
add("unicode-fullwidth", "ｍov x0, x1\nldr x2, [sp, #8]\nbl _foo")
add("r1-atomics", "ldaxr x0, [x1]\nstlxr w2, x0, [x1]\nfmov d0, d1")
add("r1-a64-families", "fadd d0, d1, d2\nbic x0, x1, x2\nmvn x3, x4\nneg x5, x6\nsxtw x7, w8\nuxtb w9, w10\nclz x11, x12\nrev x13, x14\n")
add("r1-x86-families", "sar eax, 1\nnot rbx\nxchg rax, rbx\njbe 0x10\nsete al\ncmovne rax, rbx\ncdq\nsyscall\n")
add("r1-x86-att-suffix", "addq $8, %rsp\nmovl $1, %eax\nxorl %eax, %eax\n")
add("r1-plain-b", "b .L3\nb _foo\nb 0x1000\n")
add("r1-near-bare-branches", "- b main\nb 12\njs foo\njo bar\njc baz\njp qux\nb label\nloop again\nLoop forward/back/end\n")
add("r1-near-hash-numbers", "fix #12\nrev #13\nneg #14\nnot #15\n")
add("r1-armv7-sse", "bx lr\nbeq label\nblx r3\nmovss xmm0, xmm1\nmovaps xmm2, xmm3\npxor xmm4, xmm4\n")
add("r1-pipe-joined", "mov x0, x1 | ldr x2, [sp, #8] | bl _foo\n")
add("r1-semicolon-plus-prose", "mov x0, x1; ldr x2, [sp, #8]; then we return; bl _foo\n")
add("r1-semicolon-joined", "mov x0, x1; ldr x2, [sp, #8]; bl _foo\n")
add("r1-near-semicolon-prose", "push the button; call foo; mov x0, x1\nb is a variable; not c; neg 5\n")
add("r1-near-short-words", "We not rev neg the sar value\nbic and mvn are words, as is clz\nnot\nneg\nsyscall is a word\n")
add("md-r5", "- `sub_1A2B`")
add("md-r6", "1. /System/Applications/Foo.app")
add("case-r6", "/system/library/Frameworks/x")
for i, t in enumerate(("_foo + 0x1c", "_foo+0X1C", "foo+ 0x10", "_foo+28")):
    add(f"r2-ident-{i}", t)
add("r2-near-ident", "n + 1 and a+b and 1+2\n")
add("r4-32bit-lines", "\n".join(f"0x1000000{i}" for i in range(5)))
add("r4-bare-lines", "\n".join(f"deadbe0{i}" for i in range(5)))
add("r4-32bit-oneline", "0x10000001 0x10000002 0x10000003 0x10000004 0x10000005")
add("r4-near-32bit-four", "\n".join(f"0x1000000{i}" for i in range(4)))
add("r4-near-dates", "dates 20261009 20261010 20261011 20261012 20261013\n")
add("r4-near-words", "deadbeef decade facade\n")
add("r8-near-pc-prose", "PC: 2 machines were used; lr = 3 times\n")
add("r8-pc-hex", "PC: 0x1000")
add("r4-underscore", "\n".join(f"0x0000_7ffe_0000_000{i}" for i in range(5)))
add("r4-upper", "\n".join(f"0X00007FFE0000000{i}" for i in range(5)))
add("r4-decimal", "\n".join(f"{10**17 + i}" for i in range(5)))
add("r4-blank-lines", "\n\n".join(f"0x00007ffe0000000{i}" for i in range(5)))
add("r4-near-small", "count 123456 and 98765\n12345678\n")
add("clean-spec", "# Spec\n\nRequirement 1 (rung 3): _CFFoo returns NULL for an empty key, per the public docs.\n"
    "Requirement 2 (OBSERVED, rung 4): word 0x2 seen 3 times at position 1.\nSources: developer.apple.com CFFoo docs; Libc/gen/foo.c\n")

add("r3-interface-alone", "@interface Foo : NSObject\n@end\n")
add("unicode-unit-separator", "mov\x1fx0, x1\nldr x2, [sp, #8]\nbl _foo")

# --- word boundaries: python's \w has no combining marks or connector punctuation (PCRE2 10.43+ UCP does) ---
add("r5-combining-mark", "calls sub_12ab\u0301 x\n")
add("r5-connector-punctuation", "x\u203fsub_1a x\n")
add("r5-near-composed", "a\u0301sub_1 x\n")
add("r5-near-mark-after-digit", "v1\u0301 = 2\n")
add("r2-combining-mark", "at _foo\u0301+0x1c\n")

# --- R10 (ruling F3) adversarial ---
add("r10-upper-case", "LIBSYSTEM_KERNEL.DYLIB+0X1C2\n")
add("r10-bulleted-backticks", "- `AppKit` @ 0x4f10\n")
add("r10-pc-next-line", "QuartzCore drew it\npc 0x7ff800001234\n")
add("r10-near-two-lines-apart", "QuartzCore drew it\n\npc 0x7ff800001234\n")

# --- Turkish dotted/dotless I under re.I (PCRE2 does not fold them by default) ---
add("r6-dotted-capital-i", "see /System/L\u0130brary/Frameworks for docs\n")
add("r6-dotless-small-i", "see /System/L\u0131brary/Frameworks for docs\n")
add("r10-dotless-i-lib", "see l\u0131bsystem_kernel.dylib+0x1c2\n")
# --- compliance markers past the 4096-char window (strict re-check) ---
add("long-line-sub-at-5000", "a " * 2500 + "calls sub_1A2B\n", "E4")
add("long-line-lib-offset-at-9000", "b " * 4500 + "libsystem_kernel.dylib+0x1c2\n", "E4")

# --- case folding of keys and headers (python lower()/re.I vs Qt) ---
add("r7-key-dotted-capital-i", '{"sym": "_f", "RD\u0130": 1}\n')
add("r7-key-upper", '{"sym": "_f", "RDI": 1}\n')
add("r7-key-kelvin", '{"sym": "_f", "x0": 1, "\u212aey": 2}\n')
# --- NUL / U+FFFD inside a marker: oracle no hit (store rejects R9); launcher E5 catches it ---
add("e5-nul-in-sub", "see su\x00b_1A2B here\n", "E5")
add("e5-fffd-in-sub", "see su\ufffdb_1A2B here\n", "E5")

# --- json raw_decode details (parity) ---
add("json-dup-key-last-wins", '{"a": {"symbol": "_f", "regs": []}, "a": 1}\n')
add("json-dup-key-trace-last", '{"a": 1, "a": {"symbol": "_f", "regs": []}}\n')
add("json-tab-in-string", '{"symbol": "_\tf", "regs": []}\n')
add("json-formfeed-ws", '{"symbol":\f"_f", "regs": []}\n')
add("json-escaped-keys", '{"\\u0073ymbol": "_f", "\\u0072egs": []}\n')
add("json-lone-surrogate-key", '{"symbol\\ud800": 1, "sym": "_f", "x0": 1}\n')
add("json-int-4301-digits", '{"symbol": "_f", "regs": [' + "1" * 4301 + ']}\n', "E4")
add("json-int-4300-digits", '{"symbol": "_f", "regs": [' + "1" * 4300 + ']}\n', "E4")
add("json-rescan-after-failure", '{bad {"symbol": "_f", "regs": []}\n')
add("json-continue-after-success", '{"ok": {"x": 1}} {"symbol": "_f", "regs": []}\n')
add("json-nested-200", "[" * 200 + '{"symbol": "_f", "regs": []}' + "]" * 200 + "\n")
add("json-nan-infinity", '{"symbol": "_f", "regs": [NaN, -Infinity, 1.5e10, -0.0]}\n')
add("json-trailing-comma", '{"symbol": "_f", "regs": [1,]}\n')
add("json-leading-zero", '{"symbol": "_f", "regs": [01]}\n')
add("u-fffe-noncharacter", "text \ufffe here\n")
add("long-line", "a" * 5000 + "\n", "E4")

# --- adversarial encodings (launcher brief) ---
asm = "mov x0, x1\nldr x2, [sp, #8]\nbl _foo\n"
add("adv-case-upper", asm.upper())
add("adv-case-mixed", "MoV x0, X1\nLdR x2, [SP, #8]\nBl _foo\n")
add("adv-crlf", asm.replace("\n", "\r\n"))
add("adv-table-cells", "| addr | insn |\n|---|---|\n| | mov x0, x1 |\n| | ldr x2, [sp, #8] |\n| | bl _foo |\n")
add("adv-fullwidth-sub", "calls ｓｕｂ_1A2B")          # NFKC folds
add("adv-split-trace-json", '{"symbol":\n"_f",\n"regs":\n[1]}\n')
add("adv-split-hexwords", "0x10000001\n0x10000002\n\n0x10000003\n0x10000004\n0x10000005\n")
add("adv-split-r2", "see <\n+28> here\n")                             # known bypass: split token
add("adv-cyrillic-mov", "mоv x0, x1\nldr x2, [sp, #8]\nbl _fоo\n".replace("ldr", "ldг"), "E1")
add("adv-cyrillic-sub", "calls ѕub_1A2B", "E1")
add("adv-greek-loc", "goto lοc_401000", "E1")
add("adv-base64-asm", "Attachment: " + base64.b64encode(asm.encode()).decode() + "\n", "E2")
add("adv-base64-path", base64.b64encode(b"/System/Library/Frameworks/Foo.framework/Foo").decode(), "E2")
add("adv-base64-near-sha", "Inputs: 01-research-to-spec.md " + "ab" * 32 + "\n")
add("adv-hex-escaped-sub", "calls \\x73\\x75\\x62\\x5f1A2B", "E3")
add("adv-unicode-escaped-path", "\\u002fSystem\\u002fLibrary\\u002fFoo", "E3")
add("adv-url-escaped-path", "%2FSystem%2FLibrary%2FFoo", "E3")
add("adv-html-escaped-sub", "&#115;&#117;&#98;_1A2B", "E3")
add("adv-near-prose-escapes", "Use \\n for newlines and 100% of tests; &amp; is fine.\n")
add("adv-near-russian", "Это обычный текст.\n")

# --- capture every fixture of the current oracle test suite ---
captured = {}
orig_check, orig_recon = ch.check_text, ch.recon_format
def spy_check(text, src, dst):
    captured.setdefault(text, "oracle")
    return orig_check(text, src, dst)
def spy_recon(text):
    captured[text] = "recon"
    return orig_recon(text)
ch.check_text, ch.recon_format = spy_check, spy_recon
os.environ["TMPDIR"] = os.environ.get("TMPDIR", tempfile.gettempdir())
import test_check_handoff  # noqa: E402  (same directory as the oracle)
suite = unittest.defaultTestLoader.loadTestsFromModule(test_check_handoff)
with contextlib.redirect_stderr(io.StringIO()):
    result = unittest.TextTestRunner(stream=io.StringIO()).run(suite)
ch.check_text, ch.recon_format = orig_check, orig_recon
assert result.wasSuccessful() and result.testsRun >= 78, (result.testsRun, result.failures, result.errors)
MAX_CAPTURED = 200_000  # larger inputs are rejected before the rules run, or only repeat smaller rows
for text, kind in sorted(captured.items()):
    if len(text.encode()) <= MAX_CAPTURED:
        add(f"{kind}-{hashlib.sha256(text.encode()).hexdigest()[:12]}", text, "*")  # E rules not asserted

def oracle_rules(name, text):
    hits = ch.check_text(text, "research", "spec") + (ch.recon_format(text) if name.startswith("recon-") else [])
    return sorted({r[1] for r in hits})

with open(os.path.join(out, "expected.tsv"), "w") as man:
    man.write("# file\toracle rules (check_text, plus recon_format for recon-*)\tlauncher-only E rules ('*' = not asserted)\n")
    for name, text, extra in cases:
        fn = name + ".txt"
        with open(os.path.join(out, fn), "w", encoding="utf-8", newline="") as f:
            f.write(text)
        rules = oracle_rules(name, text)
        man.write(f"{fn}\t{','.join(rules) or '-'}\t{extra or '-'}\n")
print(len(cases), "cases,", result.testsRun, "oracle tests captured")
