#!/usr/bin/env python3
"""Regenerate tests/handoff-corpus/ (run: python3 tests/handoff-corpus/generate.py) from the oracle test fixtures plus adversarial cases.
expected.tsv holds the ORACLE's rule set for each file (R/X/P/D ids) and, for E-only cases, the
launcher-only E rule expected on top. Re-run only to regenerate."""
import base64, os, sys
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

with open(os.path.join(out, "expected.tsv"), "w") as man:
    man.write("# file\toracle rules (R/X/P/D, from check_handoff.check_text)\tlauncher-only E rules\n")
    for name, text, extra in cases:
        fn = name + ".txt"
        with open(os.path.join(out, fn), "w", encoding="utf-8", newline="") as f:
            f.write(text)
        rules = sorted({r[1] for r in ch.check_text(text, "research", "spec")})
        man.write(f"{fn}\t{','.join(rules) or '-'}\t{extra or '-'}\n")
print(len(cases), "cases")
