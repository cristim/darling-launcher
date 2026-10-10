"""Tests for check_handoff.py. All fixtures are short synthetic strings."""
import contextlib
import hashlib
import itertools
import io
import json
import os
import re
import stat
import tempfile
import unittest

import check_handoff as ch


def put(path, text):
    with open(path, "w") as f:
        f.write(text)


def rules(text, src="research", dst="spec"):
    return {r[1] for r in ch.check_text(text, src, dst)}


class RuleTests(unittest.TestCase):
    def test_r1_consecutive_run_rejected(self):
        self.assertIn("R1", rules("mov x0, x1\nldr x2, [sp, #8]\nbl _foo\n"))

    def test_r1_x86_block_of_six_nonconsecutive(self):
        t = "push rbp\nnote\nmov rbp, rsp\nnote\nlea rax, [rbp-8]\nnote\ncall 0x10\nnote\npop rbp\nnote\nret\n"
        self.assertIn("R1", rules(t))

    def test_r10_library_offset_forms(self):
        for x in ("libsystem_kernel.dylib+0x1c2", "AppKit + 0x4f10", "libobjc.A.dylib:0x10",
                  "AppKit crashed at offset 0x4f10", "in Foo.framework @ 0x1a0", "CoreFoundation at 0x7ff800001000",
                  "- `libdispatch.dylib` + 0x20"):
            self.assertIn("R10", rules(x), x)

    def test_r10_pc_lr_values_next_to_library(self):
        self.assertIn("R10", rules("pc = 0x7ff800001234 in libsystem_c\n"))
        self.assertIn("R10", rules("crash in AppKit\nlr: 0x7ff800002000\n"))
        self.assertIn("R10", rules("return address 0x7ff800003000\nlibobjc.A.dylib\n"))
        self.assertIn("R10", rules("frame 0x7ff800003000 (QuartzCore)\n"))

    def test_r10_near_misses(self):
        for x in ("AppKit and libobjc are public libraries.\n", "libobjc.A.dylib is linked via otool -L\n",
                  "NSWindow, objc_msgSend are public symbols; AppKit exports NSWindow.\n",
                  "The pc and lr registers hold addresses; AppKit draws windows.\n",
                  "our build: main at 0x401000 and pc 0x401234 in our own test binary\n",
                  "AppKit flag 0x10 is documented\n", "frame 0x7ff800003000 in our harness\n"):
            self.assertNotIn("R10", rules(x), x)

    def test_zero_width_chars_do_not_split_tokens(self):
        self.assertIn("R5", rules("see su\u200db_1A2B here"))  # ZWJ inside sub_
        self.assertIn("R5", rules("see sub\u200b_1A2B here"))  # ZWSP
        t = "m\u200bov x0, x1\nl\u2060dr x2, [sp, #8]\nbl\u00ad _foo\n"  # ZWSP, word joiner, soft hyphen
        self.assertIn("R1", rules(t))

    def test_lone_cr_nel_ls_ps_are_line_breaks(self):
        asm = ["mov x0, x1", "ldr x2, [sp, #8]", "bl _foo"]
        for br in ("\r", "\x85", "\u2028", "\u2029", "\r\n"):
            self.assertIn("R1", rules(br.join(asm)), repr(br))
            self.assertIn("R5", rules("prose" + br + "sub_1A2B"), repr(br))
        hits = ch.check_text("a\rb\rsub_1A2B", "spec", "impl")
        self.assertEqual(3, hits[0][0])

    def test_r1_near_miss_prose_and_short_runs(self):
        self.assertEqual(set(), rules("We mov the file, then call the function and push the button.\n"))
        self.assertEqual(set(), rules("mov x0, x1\nldr x2, [sp, #8]\n"))  # only 2 lines
        self.assertEqual(set(), rules("add 5\nsub 3\nand then\ncall foo bar\n"))

    def test_r2_address_offset(self):
        for t in ("see <+28> here", "0x100003f40: foo", "at main+0x1c", "<_main+28>"):
            self.assertIn("R2", rules(t), t)

    def test_r2_near_miss(self):
        self.assertEqual(set(), rules("version 1+2 is fine; 0x10 is a flag; main + 1\n"))

    def test_r3_tool_signatures(self):
        for t in ("Contents of (__TEXT,__text) section", "foo: file format mach-o arm64",
                  "Idx Name Size VMA", "    cmd LC_SEGMENT_64"):
            self.assertIn("R3", rules(t), t)

    def test_r3_classdump_needs_interface_and_ivar_offsets(self):
        dump = "@interface Foo : NSObject\n{\n    int _a;  // 8 = 0x8\n}\n@end\n"
        self.assertIn("R3", rules(dump))

    def test_r3_near_miss(self):
        self.assertEqual(set(), rules("The @interface keyword declares a class.\n"))
        self.assertEqual(set(), rules("int _a;  // 8 = 0x8\n"))  # no @interface
        self.assertEqual(set(), rules("otool -L output: /usr/lib/libSystem.B.dylib (compatibility version 1.0.0)\n"))

    def test_r4_hexdump_line(self):
        self.assertIn("R4", rules("00001000  de ad be ef 00 11 22 33  44\n"))
        self.assertIn("R4", rules("00000010: 4865 6c6c 6f20 776f 726c\n"))

    def test_r4_word_run(self):
        five = "\n".join(f"0x00007ffe0000000{i}" for i in range(ch.MAX_HEX_WORDS + 1))
        self.assertIn("R4", rules(five))
        self.assertIn("R4", rules("0x00007ffe00000001 0x00007ffe00000002 0x00007ffe00000003 "
                                  "0x00007ffe00000004 0x00007ffe00000005"))

    def test_r4_near_miss(self):
        four = "\n".join(f"0x00007ffe0000000{i}" for i in range(ch.MAX_HEX_WORDS))
        self.assertEqual(set(), rules(four))
        sep = "0x00007ffe00000001\ntext\n0x00007ffe00000002\n" * 3
        self.assertEqual(set(), rules(sep))
        self.assertEqual(set(), rules("sha " + "a" * 64 + "\nflag 0xff 0x10\n"))

    def test_r5_markers(self):
        for t in ("calls sub_1A2B", "goto loc_401000", "x = LODWORD(y)", "v12 = a1", "__int64 f", "_QWORD *p"):
            self.assertIn("R5", rules(t), t)

    def test_r5_near_miss(self):
        self.assertEqual(set(), rules("the sub_system loc is vague; v == 3; version = 2\n"))

    def test_r6_paths(self):
        for t in ("/System/Library/Frameworks/Foo.framework/Foo", "/Volumes/Macintosh HD/System/x".replace(" ", ""),
                  "dyld_shared_cache_arm64e", "libfoo.dyld", "/usr/lib/system/libsystem_c.dylib",
                  "/Library/Apple/usr/bin"):
            self.assertIn("R6", rules(t), t)

    def test_r6_near_miss(self):
        self.assertEqual(set(), rules("Foo.framework, libSystem.B.dylib, /usr/lib/libSystem.B.dylib, ~/prefix/Library\n"))

    TRACE = '{"symbol": "_foo", "lib": "libx", "regs": [1, 2]}\n'

    def test_r7_trace_line_on_every_transition(self):
        self.assertIn("R7", rules(self.TRACE, "research", "spec"))
        self.assertIn("R7", rules(self.TRACE, "spec", "impl"))
        self.assertIn("R7", rules(self.TRACE, "research", "impl"))
        self.assertIn("R7", rules(self.TRACE, "spec", "validate"))
        self.assertIn("R7", rules('{"sym": "_f", "x0": 5}', "spec", "impl"))

    def test_r7_near_miss(self):
        self.assertEqual(set(), rules('{"title": "notes", "count": 3}\n', "spec", "impl"))
        self.assertEqual(set(), rules('{"symbol": "_foo"}\n', "spec", "impl"))
        self.assertEqual(set(), rules('{"name": "x", "args": [1]}\n', "spec", "impl"))
        self.assertEqual(set(), rules("braces { like } this and {not json}\n", "spec", "impl"))

    def test_r7_multiline_bulleted_nested_inline(self):
        cases = ['{\n  "symbol": "_f",\n  "regs": [1,\n 2]\n}\n',
                 '- {"symbol": "_f", "x0": 1}\n', '1. `{"sym": "_f", "regs": []}`\n',
                 '| a | {"symbol": "_f", "words": [1]} |\n',
                 '[{"ok": 1}, {"symbol": "_f", "arg_regs": []}]\n',
                 '{"outer": {"inner": [{"symbol": "_f", "regs": []}]}}\n',
                 'text {"name": "_f", "regs": [1]} text\n',
                 '\ufeff{"symbol": "_f", "regs": []}\n']
        for c in cases:
            self.assertIn("R7", rules(c, "research", "spec"), c)

    def test_r7_brace_flood_fails_closed(self):
        self.assertIn("R7", rules('{"a":' * (ch.MAX_JSON_FAILURES + 5)))

    def test_r8_forbidden_keys_everywhere(self):
        for k in ("lr", "pc", "backtrace", "image_offset", "return_address"):
            self.assertIn("R8", rules(f'{{"{k}": 1}}'), k)

    def test_r8_nested_array_yaml_and_case(self):
        for t in ('{"a": [{"b": {"LR": 1}}]}', "lr: 0x1c", "PC = 0x10", "backtrace: [", '"Return_Address": 5'):
            self.assertIn("R8", rules(t), t)

    def test_r8_near_miss(self):
        self.assertEqual(set(), rules("The pc and lr registers are not recorded; backtrace is forbidden.\n"))

    def test_hardening_markdown_prefixes_and_unicode(self):
        for pre in ("- ", "* ", "1. ", "2) ", "> ", "`"):
            suf = "`" if pre == "`" else ""
            t = "\n".join(f"{pre}{l}{suf}" for l in ("MOV X0, X1", "ldr x2, [sp, #8]", "bl _foo"))
            self.assertIn("R1", rules(t), pre)
        self.assertIn("R1", rules("mov\u00a0x0, x1\nldr x2, [sp, #8]\nbl _foo"))  # NBSP
        self.assertIn("R1", rules("\uff4dov x0, x1\nldr x2, [sp, #8]\nbl _foo"))  # fullwidth m
        self.assertIn("R1", rules("ldaxr x0, [x1]\nstlxr w2, x0, [x1]\nfmov d0, d1"))
        self.assertIn("R5", rules("- `sub_1A2B`"))
        self.assertIn("R6", rules("1. /System/Applications/Foo.app"))
        self.assertIn("R6", rules("/system/library/Frameworks/x"))

    def test_hardening_r2_ident_plus_offset(self):
        for t in ("_foo + 0x1c", "_foo+0X1C", "foo+ 0x10", "_foo+28"):
            self.assertIn("R2", rules(t), t)
        self.assertEqual(set(), rules("n + 1 and a+b and 1+2\n"))

    def test_r4_32bit_words_counted(self):
        five = "\n".join(f"0x1000000{i}" for i in range(5))
        self.assertIn("R4", rules(five))
        self.assertIn("R4", rules("\n".join(f"deadbe0{i}" for i in range(5))))
        self.assertIn("R4", rules("0x10000001 0x10000002 0x10000003 0x10000004 0x10000005"))
        self.assertEqual(set(), rules("\n".join(f"0x1000000{i}" for i in range(4))))
        self.assertEqual(set(), rules("dates 20261009 20261010 20261011 20261012 20261013\n"))
        self.assertEqual(set(), rules("deadbeef decade facade\n"))

    def test_r8_pc_prose_not_matched(self):
        self.assertEqual(set(), rules("PC: 2 machines were used; lr = 3 times\n"))
        self.assertIn("R8", rules("PC: 0x1000"))

    def test_hardening_r4_variants(self):
        sep = "\n".join(f"0x0000_7ffe_0000_000{i}" for i in range(5))
        self.assertIn("R4", rules(sep))
        self.assertIn("R4", rules("\n".join(f"0X00007FFE0000000{i}" for i in range(5))))
        self.assertIn("R4", rules("\n".join(f"{10**17 + i}" for i in range(5))))
        self.assertIn("R4", rules("\n\n".join(f"0x00007ffe0000000{i}" for i in range(5))))  # blank lines
        self.assertEqual(set(), rules("count 123456 and 98765\n12345678\n"))

    def test_clean_spec_prose_accepted(self):
        spec = ("# Spec\n\nRequirement 1 (rung 3): _CFFoo returns NULL for an empty key, per the public docs.\n"
                "Requirement 2 (OBSERVED, rung 4): word 0x2 seen 3 times at position 1.\n"
                "Sources: developer.apple.com CFFoo docs; Libc/gen/foo.c\n")
        self.assertEqual(set(), rules(spec, "spec", "impl"))

    def test_agent_and_inputs_parsed(self):
        t = "Agent: res-1\nInputs: 01-a.md; 02-b.md\n"
        self.assertEqual("res-1", ch.parse_header(t, "Agent"))
        self.assertEqual(["01-a.md", "02-b.md"], ch.parse_list(t, "Inputs"))
        self.assertIsNone(ch.parse_header("x\n", "Agent"))

    def test_sources_parsed(self):
        self.assertEqual(["a b", "c"], ch.parse_sources("x\nSources: a b; c\n"))
        self.assertEqual([], ch.parse_sources("none\n"))


class CliBase(unittest.TestCase):
    def setUp(self):
        self._td = tempfile.TemporaryDirectory()
        self.addCleanup(self._td.cleanup)
        self.root = os.path.join(self._td.name, "run")
        self.hand = os.path.join(self.root, "handoff")
        os.mkdir(self.root, 0o700)

    def write(self, text, stage="research", name="in.md"):
        os.makedirs(os.path.join(self.root, stage), mode=0o700, exist_ok=True)
        p = os.path.join(self.root, stage, name)
        with open(p, "w") as f:
            f.write(text)
        return p

    def call(self, argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(out):
            rc = ch.main(argv)
        return rc, err.getvalue(), out.getvalue()

    _n = itertools.count()

    def run_cli(self, src, dst, text, name="in.md", root=None, unique=True, extra=(), agent=False):
        if unique:  # identical content is refused (D1), so give each file its own tail
            text += f"\nnote {next(self._n)}\n"
        p = self.write(text, src, name)
        extra = list(extra)
        if src in ch.NEEDS_AGENT and "--agent-id" not in extra and not agent:
            extra += ["--agent-id", "test-agent"]
        rc, err, _ = self.call(["--run-root", root or self.root, "--from", src, "--to", dst, "--file", p, *extra])
        return rc, err

    def verify(self):
        rc, err, out = self.call(["--verify-chain", "--run-root", self.root])
        self.last_out = out
        return rc, err

    def audit(self):
        with open(os.path.join(self.hand, "AUDIT.jsonl")) as f:
            return [json.loads(l) for l in f]

    def listing(self):
        return sorted(n for n in os.listdir(self.hand) if n != "AUDIT.jsonl")


class CliTests(CliBase):
    CLEAN = "Behaviour spec.\nSources: public docs; Libc\n"

    def test_release_and_sequence(self):
        self.assertEqual(0, self.run_cli("research", "spec", self.CLEAN)[0])
        self.assertEqual(0, self.run_cli("spec", "impl", self.CLEAN)[0])
        self.assertEqual(["01-research-to-spec.md", "02-spec-to-impl.md", "MANIFEST.txt"], self.listing())
        with open(os.path.join(self.hand, "01-research-to-spec.md")) as f:
            self.assertTrue(f.read().startswith(self.CLEAN))
        recs = self.audit()
        self.assertEqual(["RELEASE", "RELEASE"], [r["verdict"] for r in recs])
        self.assertEqual(["public docs", "Libc"], recs[0]["sources"])
        self.assertEqual(ch.GENESIS, recs[0]["prev_hash"])
        self.assertIsNone(recs[0]["agent"])
        self.assertEqual([], recs[0]["inputs"])
        self.assertEqual(0, self.verify()[0])

    def test_agent_inputs_logged(self):
        self.run_cli("research", "spec", "Agent: a7\nInputs: 01-x.md\n")
        r = self.audit()[0]
        self.assertEqual(("a7", ["01-x.md"]), (r["agent"], r["inputs"]))

    def test_transitions(self):
        for src, dst in [("research", "impl"), ("research", "validate"), ("spec", "research"),
                         ("impl", "research"), ("impl", "impl"), ("research", "research"),
                         ("validate", "research"), ("validate", "validate")]:
            rc, err = self.run_cli(src, dst, self.CLEAN)
            self.assertEqual(1, rc, (src, dst))
            self.assertIn("[X1]", err)
        for src, dst in [("research", "spec"), ("spec", "impl"), ("impl", "validate"), ("spec", "validate"),
                         ("impl", "spec"), ("validate", "impl"), ("validate", "spec")]:
            self.assertEqual(0, self.run_cli(src, dst, self.CLEAN)[0], (src, dst))

    def test_reject_leaves_handoff_unchanged_and_logs(self):
        self.run_cli("research", "spec", self.CLEAN)
        before = self.listing()
        rc, err = self.run_cli("spec", "impl", "see sub_1234 here\nmore\n0x100003f40: x\n")
        self.assertEqual(1, rc)
        self.assertIn(":1: [R5]", err)
        self.assertIn(":3: [R2]", err)
        self.assertNotIn("sub_1234", err)  # matched content is never echoed
        self.assertEqual(before, self.listing())
        last = self.audit()[-1]
        self.assertEqual("REJECT", last["verdict"])
        self.assertEqual(["R2", "R5"], last["rules"])
        self.assertIsNone(last["output_sha256"])
        self.assertEqual(0, self.verify()[0])

    def test_permissions(self):
        self.run_cli("research", "spec", self.CLEAN)
        mode = lambda p: stat.S_IMODE(os.stat(p).st_mode)
        self.assertEqual(0o700, mode(self.hand))
        for n in ("AUDIT.jsonl", "MANIFEST.txt", "01-research-to-spec.md"):
            self.assertEqual(0o600, mode(os.path.join(self.hand, n)), n)

    def test_symlinked_handoff_dir_refused(self):
        target = os.path.join(self._td.name, "elsewhere")
        os.mkdir(target, 0o700)
        os.symlink(target, self.hand)
        rc, err = self.run_cli("research", "spec", self.CLEAN)
        self.assertEqual(2, rc)
        self.assertIn("symlink", err)
        self.assertEqual([], os.listdir(target))

    def test_symlinked_audit_file_refused(self):
        os.mkdir(self.hand, 0o700)
        target = os.path.join(self._td.name, "stolen.jsonl")
        open(target, "w").close()
        os.symlink(target, os.path.join(self.hand, "AUDIT.jsonl"))
        rc, _ = self.run_cli("research", "spec", self.CLEAN)
        self.assertEqual(2, rc)
        self.assertEqual(0, os.path.getsize(target))
        self.assertEqual([], [n for n in os.listdir(self.hand) if n.endswith(".md")])

    def test_symlinked_run_root_refused_even_with_trailing_slash(self):
        self.write(self.CLEAN)
        link = os.path.join(self._td.name, "link")
        os.symlink(self.root, link)
        for r in (link, link + "/"):
            rc, err = self.run_cli("research", "spec", self.CLEAN, root=r)
            self.assertEqual(2, rc, r)
            self.assertIn("symlink", err)
        self.assertFalse(os.path.exists(self.hand))

    def test_file_from_other_stage_or_outside_refused(self):
        other = self.write(self.CLEAN, "research", "T1.md")
        self.write(self.CLEAN, "spec", "ok.md")
        rc, err, _ = self.call(["--run-root", self.root, "--from", "spec", "--to", "impl", "--file", other])
        self.assertEqual(2, rc)
        self.assertIn("outside the spec workspace", err)
        outside = os.path.join(self._td.name, "o.md")
        put(outside, self.CLEAN)
        rc, _, _ = self.call(["--run-root", self.root, "--from", "spec", "--to", "impl", "--file", outside])
        self.assertEqual(2, rc)
        sneaky = os.path.join(self.root, "spec", "..", "research", "T1.md")
        rc, _, _ = self.call(["--run-root", self.root, "--from", "spec", "--to", "impl", "--file", sneaky])
        self.assertEqual(2, rc)
        self.assertFalse(os.path.exists(self.hand))

    def test_symlinked_input_refused(self):
        secret = self.write(self.CLEAN, "research", "T1.md")
        os.makedirs(os.path.join(self.root, "spec"), mode=0o700, exist_ok=True)
        link = os.path.join(self.root, "spec", "l.md")
        os.symlink(secret, link)
        rc, err, _ = self.call(["--run-root", self.root, "--from", "spec", "--to", "impl", "--file", link])
        self.assertEqual(2, rc)

    def test_typo_or_stageless_run_root_refused_without_init_hint(self):
        typo = os.path.join(self._td.name, "typo-run")
        p = os.path.join(self._td.name, "f.md")
        put(p, self.CLEAN)
        rc, err, _ = self.call(["--run-root", typo, "--from", "research", "--to", "spec", "--file", p])
        self.assertEqual(2, rc)
        self.assertFalse(os.path.exists(typo))
        empty = os.path.join(self._td.name, "empty")
        os.mkdir(empty, 0o700)
        rc, err2, _ = self.call(["--run-root", empty, "--from", "research", "--to", "spec", "--file", p])
        self.assertEqual(2, rc)
        for e in (err, err2):
            self.assertNotIn("--init", e)
        self.assertFalse(os.path.exists(os.path.join(empty, "staging")))
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            ch.main(["--run-root", empty, "--init", "--from", "research", "--to", "spec", "--file", p])

    def test_group_world_accessible_dirs_refused(self):
        os.chmod(self.root, 0o755)
        rc, err = self.run_cli("research", "spec", self.CLEAN)
        self.assertEqual(2, rc)
        self.assertIn("group/world", err)
        os.chmod(self.root, 0o700)
        self.write(self.CLEAN, "research", "x.md")
        os.chmod(os.path.join(self.root, "research"), 0o755)
        rc, err = self.run_cli("research", "spec", self.CLEAN)
        self.assertEqual(2, rc)
        self.assertIn("group/world", err)
        os.chmod(os.path.join(self.root, "research"), 0o700)
        os.mkdir(self.hand, 0o777)
        os.chmod(self.hand, 0o777)
        rc, err = self.run_cli("research", "spec", self.CLEAN)
        self.assertEqual(2, rc)
        self.assertIn("group/world", err)

    def test_stray_nn_file_and_corrupt_audit_exit_2_no_traceback(self):
        self.assertEqual(0, self.run_cli("research", "spec", self.CLEAN)[0])
        stray = os.path.join(self.hand, "02-spec-to-impl.md")
        put(stray, "x")
        rc, err = self.run_cli("spec", "impl", self.CLEAN)
        self.assertEqual(2, rc)
        self.assertIn("stray file", err)
        self.assertNotIn("Traceback", err)
        self.assertEqual(1, self.verify()[0])
        os.remove(stray)
        with open(os.path.join(self.hand, "AUDIT.jsonl"), "a") as f:
            f.write("not json\n")
        rc, err = self.run_cli("spec", "impl", self.CLEAN)
        self.assertEqual(2, rc)
        self.assertIn("corrupt", err)
        self.assertEqual(1, self.verify()[0])

    def test_non_ascii_digit_file_name_is_a_stray_file(self):
        self.assertEqual(0, self.run_cli("research", "spec", self.CLEAN)[0])
        stray = os.path.join(self.hand, "\u0660\u0662-spec-to-impl.md")  # Arabic-Indic digits
        put(stray, "x")
        rc, err = self.verify()
        self.assertEqual((1, True), (rc, "stray file" in err))
        rc, err = self.run_cli("spec", "impl", self.CLEAN)
        self.assertEqual((2, True), (rc, "stray file" in err))
        os.remove(stray)
        put(os.path.join(self.hand, "notes.txt"), "x")  # any unrecognised name is stray
        self.assertEqual(1, self.verify()[0])

    def test_failed_release_leaves_no_nn_file(self):
        self.assertEqual(0, self.run_cli("research", "spec", self.CLEAN)[0])
        mf = os.path.join(self.hand, "MANIFEST.txt")
        os.remove(mf)
        os.symlink("/dev/null", mf)
        before = self.listing()
        rc, err = self.run_cli("spec", "impl", self.CLEAN)
        self.assertEqual(2, rc)
        self.assertEqual(before, self.listing())
        self.assertEqual(1, len(self.audit()))

    def test_unreadable_oversize_and_directory_input_exit_2(self):
        big = self.write("a\n" * ch.MAX_INPUT, "research", "big.md")
        p = os.path.join(self.root, "research", "sub")
        os.mkdir(p, 0o700)
        for f in (big, p, os.path.join(self.root, "research", "missing.md")):
            rc, _, _ = self.call(["--run-root", self.root, "--from", "research", "--to", "spec", "--file", f])
            self.assertEqual(2, rc, f)

    def test_binary_input_rejected(self):
        p = self.write("", "research", "b.bin")
        with open(p, "wb") as f:
            f.write(b"abc\0def")
        rc, err, _ = self.call(["--run-root", self.root, "--from", "research", "--to", "spec", "--file", p])
        self.assertEqual(1, rc)
        self.assertIn("[R9]", err)


class ChainTests(CliBase):
    def setUp(self):
        super().setUp()
        for src, dst in [("research", "spec"), ("spec", "impl"), ("impl", "validate")]:
            self.assertEqual(0, self.run_cli(src, dst, f"clean {src}\nSources: docs\n")[0])
        self.run_cli("spec", "impl", "sub_ABCD\n")  # trailing rejection record
        self.assertEqual(0, self.verify()[0])

    def test_modified_released_file_detected(self):
        p = os.path.join(self.hand, "02-spec-to-impl.md")
        os.chmod(p, 0o600)
        with open(p, "a") as f:
            f.write("tampered\n")
        rc, err = self.verify()
        self.assertEqual(1, rc)
        self.assertIn("02-spec-to-impl.md was modified", err)

    def test_deleted_middle_audit_line_detected(self):
        p = os.path.join(self.hand, "AUDIT.jsonl")
        with open(p) as f:
            lines = f.readlines()
        del lines[1]
        with open(p, "w") as f:
            f.writelines(lines)
        rc, err = self.verify()
        self.assertEqual(1, rc)
        self.assertIn("prev_hash mismatch", err)

    def test_edited_audit_line_detected(self):
        p = os.path.join(self.hand, "AUDIT.jsonl")
        with open(p) as f:
            lines = f.readlines()
        lines[0] = lines[0].replace('"docs"', '"other"')
        with open(p, "w") as f:
            f.writelines(lines)
        self.assertEqual(1, self.verify()[0])

    def test_deleted_last_release_record_detected(self):
        p = os.path.join(self.hand, "AUDIT.jsonl")
        with open(p) as f:
            lines = f.readlines()
        with open(p, "w") as f:
            f.writelines(lines[:2] + lines[3:])  # drop release 3, keep trailing reject
        rc, err = self.verify()
        self.assertEqual(1, rc)

    def test_orphan_released_file_detected(self):
        p = os.path.join(self.hand, "AUDIT.jsonl")
        with open(p) as f:
            lines = f.readlines()
        with open(p, "w") as f:
            f.writelines(lines[:3])  # drop only the trailing reject, release 3 stays
        self.assertEqual(0, self.verify()[0])  # a trailing REJECT removal is a documented limit
        with open(p, "w") as f:
            f.writelines(lines[:2])
        rc, err = self.verify()
        self.assertEqual(1, rc)
        self.assertIn("no audit record", err)

    def test_deleted_released_file_detected(self):
        os.remove(os.path.join(self.hand, "01-research-to-spec.md"))
        rc, err = self.verify()
        self.assertEqual(1, rc)
        self.assertIn("missing", err)


class ComplianceTests(CliBase):
    def test_r1_r3_r5_hits_flagged_and_reported(self):
        for i, (text, rid) in enumerate([("mov x0, x1\nldr x2, [sp, #8]\nbl _foo\n", "R1"),
                                         ("Contents of (__TEXT,__text) section\n", "R3"),
                                         ("see sub_1A2B\n", "R5")]):
            rc, err = self.run_cli("spec", "impl", text)
            self.assertEqual(1, rc)
            self.assertIn(f"REPORT-TO-COMPLIANCE: rule {rid}", err)
            self.assertTrue(self.audit()[i]["report_to_compliance"], rid)

    def test_r10_is_reported_to_compliance(self):
        rc, err = self.run_cli("spec", "impl", "see libsystem_kernel.dylib+0x1c2\n")
        self.assertEqual(1, rc)
        self.assertIn("REPORT-TO-COMPLIANCE: rule R10", err)
        self.assertTrue(self.audit()[-1]["report_to_compliance"])

    def test_compliance_report_printed_even_when_store_fails(self):
        self.assertEqual(0, self.run_cli("research", "spec", "ok\n")[0])
        audit = os.path.join(self.hand, "AUDIT.jsonl")
        with open(audit, "a") as f:
            f.write("garbage\n")  # corrupt audit: the REJECT cannot be recorded
        rc, err = self.run_cli("spec", "impl", "see sub_1A2B and libobjc.A.dylib+0x1c\n")
        self.assertEqual(2, rc)
        self.assertIn("error:", err)
        self.assertIn("REPORT-TO-COMPLIANCE: rule R5", err)
        self.assertIn("REPORT-TO-COMPLIANCE: rule R10", err)
        # crashed-release leftover: refuse_broken aborts before any record
        with open(audit) as f:
            keep = f.readlines()[:-1]
        with open(audit, "w") as f:
            f.writelines(keep)
        put(os.path.join(self.hand, ".tmp-02-spec-to-impl.md"), "x")
        rc, err = self.run_cli("spec", "impl", "see sub_1A2B\n")
        self.assertEqual(2, rc)
        self.assertIn("REPORT-TO-COMPLIANCE: rule R5", err)
        # a clean file that aborts prints no report
        rc, err = self.run_cli("spec", "impl", "clean\n")
        self.assertEqual((2, False), (rc, "REPORT-TO-COMPLIANCE" in err))

    def test_report_to_compliance_must_be_json_boolean(self):
        self.run_cli("spec", "impl", "see sub_1A2B\n")  # flagged true
        self.assertEqual(0, self.run_cli("research", "spec", "ok\n")[0])
        audit = os.path.join(self.hand, "AUDIT.jsonl")
        for bad in ('1', '"yes"', '"true"'):
            with open(audit) as f:
                lines = f.readlines()
            rec = json.loads(lines[-1])
            rec["report_to_compliance"] = json.loads(bad)
            with open(audit, "w") as f:
                f.writelines(lines[:-1] + [json.dumps(rec, sort_keys=True) + "\n"])
            rc, err = self.verify()
            self.assertEqual(1, rc, bad)
            self.assertIn("not a JSON boolean", err)
            rc, _, out = self.call(["--summary", "--run-root", self.root])
            self.assertEqual(0, rc)
            self.assertIn("compliance_reports=1 ", out)  # only the literal true on line 1 counts

    def test_other_hits_not_flagged(self):
        for text in ("/System/Library/Foo\n", "\n".join(f"0x1000000{i}" for i in range(5)),
                     '{"symbol": "_f", "regs": []}\n'):
            rc, err = self.run_cli("research", "spec", text)
            self.assertEqual(1, rc)
            self.assertNotIn("REPORT-TO-COMPLIANCE", err)
        self.assertEqual([False] * 3, [r["report_to_compliance"] for r in self.audit()])

    def test_summary_counts_and_prefix(self):
        self.run_cli("spec", "impl", "see sub_1A2B\n")
        self.assertEqual(0, self.run_cli("research", "spec", "ok\n")[0])
        os.mkdir(os.path.join(self.root, "spec.tainted"))
        os.mkdir(os.path.join(self.root, "spec.tainted", "agent-a"))
        os.mkdir(os.path.join(self.root, "spec.tainted", "agent-b"))  # second taint of the same stage
        os.mkdir(os.path.join(self.root, "impl.tainted"))  # bare: counts 0, warns
        os.mkdir(os.path.join(self.root, "notes.tainted.bak"))
        os.mkdir(os.path.join(self.root, "notes.tainted.bak", "x"))
        open(os.path.join(self.root, "spec.tainted", "file"), "w").close()  # a file, not an agent dir
        rc, err = self.verify()
        self.assertEqual(0, rc)
        self.assertIn("impl.tainted has no <agent-id> subdirectory", err)
        self.assertNotIn("spec.tainted has no", err)
        m = re.fullmatch(r"records=2 last_hash=([0-9a-f]{64}) tainted_stages=2 compliance_reports=1 recon_releases=0\n", self.last_out)
        self.assertTrue(m, self.last_out)
        rc, _, out = self.call(["--summary", "--run-root", self.root])
        self.assertEqual((0, self.last_out), (rc, out))


class TaintedReconTests(CliBase):
    def test_recon_tainted_agent_dir_is_counted(self):
        self.assertEqual(0, self.run_cli("research", "spec", "ok\n")[0])
        os.makedirs(os.path.join(self.root, "recon.tainted", "reconciler-1"))
        rc, err = self.verify()
        self.assertEqual(0, rc)
        self.assertIn("tainted_stages=1 ", self.last_out)
        self.assertNotIn("warning", err)

    def test_bare_recon_tainted_warns_and_counts_zero(self):
        self.assertEqual(0, self.run_cli("research", "spec", "ok\n")[0])
        os.mkdir(os.path.join(self.root, "recon.tainted"))
        rc, err = self.verify()
        self.assertEqual(0, rc)
        self.assertIn("tainted_stages=0 ", self.last_out)
        self.assertIn("recon.tainted has no <agent-id> subdirectory", err)


class ProvenanceTests(CliBase):
    def test_input_must_be_released_to_the_from_stage(self):
        t1 = self.release_one()  # research->spec
        self.assertEqual(0, self.run_cli("spec", "impl", "spec\n")[0])
        for extra, body in ((["--inputs", t1["output"]], "note\n"),
                            ([], f"Inputs: {t1['output']} {t1['output_sha256']}\n")):
            rc, err = self.run_cli("impl", "validate", body, extra=extra, agent=False)
            self.assertEqual(1, rc, extra)
            self.assertIn("[P2]", err)

    def test_inputs_header_forms(self):
        t1 = self.release_one()
        n, h = t1["output"], t1["output_sha256"]
        bad = [f"Inputs: {n}", f"Inputs: {h}", f"Inputs: {n} {h[:63]}", f"Inputs: nosuch.md {h}",
               f"Inputs: 05-research-to-spec.md {h}", f"Inputs: {n} {h}; free text",
               f"Inputs: {n} {'0' * 64}"]
        for b in bad:
            rc, err = self.run_cli("spec", "impl", b + "\n")
            self.assertEqual((1, True), (rc, "[P2]" in err), b)
        for ok in (f"Inputs: {n} {h}", f"Inputs: {n} {h.upper()}"):
            self.assertEqual(0, self.run_cli("spec", "impl", ok + "\n")[0], ok)

    def test_research_inputs_header_is_free_form_but_agent_optional(self):
        self.assertEqual(0, self.run_cli("research", "spec", "Inputs: research/trace/ (T0)\n")[0])

    def test_agent_required_from_impl_onward(self):
        self.release_one()
        self.assertEqual(0, self.run_cli("spec", "impl", "no agent\n")[0])  # spec: allowed
        for src, dst in (("impl", "validate"), ("validate", "impl")):
            p = self.write("x\n", src, f"{src}.md")
            rc, err, _ = self.call(["--run-root", self.root, "--from", src, "--to", dst, "--file", p])
            self.assertEqual((1, True), (rc, "[P3]" in err), src)
        p = self.write("Agent: h1\n", "impl", "h.md")
        self.assertEqual(0, self.call(["--run-root", self.root, "--from", "impl", "--to", "validate",
                                       "--file", p])[0])

    def test_retry_after_crash_before_link_is_exit_2_without_new_record(self):
        self.release_one()
        self.assertEqual(0, self.run_cli("spec", "impl", "second\n")[0])
        os.remove(os.path.join(self.hand, "02-spec-to-impl.md"))  # crash before the link
        put(os.path.join(self.hand, ".tmp-02-spec-to-impl.md"), "second\n")
        n = len(self.audit())
        rc, err = self.run_cli("spec", "impl", "second\n", unique=False)
        self.assertEqual((2, True), (rc, "leftover temp file" in err))
        os.remove(os.path.join(self.hand, ".tmp-02-spec-to-impl.md"))
        rc, err = self.run_cli("spec", "impl", "second\n", unique=False)
        self.assertEqual((2, True), (rc, "is missing" in err))
        self.assertEqual(n, len(self.audit()))

    def release_one(self, src="research", dst="spec", body="facts\n"):
        self.assertEqual(0, self.run_cli(src, dst, body)[0])
        recs = [r for r in self.audit() if r["verdict"] == "RELEASE"]
        return recs[-1]

    def test_flags_written_to_audit(self):
        first = self.release_one()
        rc, err = self.run_cli("spec", "impl", "spec\n", extra=["--agent-id", "ag7", "--inputs", first["output"]])
        self.assertEqual(0, rc, err)
        r = self.audit()[-1]
        self.assertEqual(("ag7", [first["output"]], [first["output_sha256"]]),
                         (r["agent"], r["inputs"], r["input_hashes"]))

    def test_flag_header_disagreement_rejected(self):
        first = self.release_one()
        rc, err = self.run_cli("spec", "impl", "Agent: a\n", extra=["--agent-id", "b"])
        self.assertEqual(1, rc)
        self.assertIn("[P1]", err)
        rc, err = self.run_cli("spec", "impl", f"Inputs: 09-spec-to-impl.md {first['output_sha256']}\n",
                               extra=["--inputs", first["output"]])
        self.assertEqual(1, rc)
        self.assertIn("[P1]", err)

    def test_agreeing_flag_and_header_accepted(self):
        first = self.release_one()
        rc, err = self.run_cli("spec", "impl", f"Agent: a\nInputs: {first['output']} {first['output_sha256']}\n",
                               extra=["--agent-id", "a", "--inputs", first["output"]])
        self.assertEqual(0, rc, err)

    def test_unknown_input_name_or_hash_rejected(self):
        first_name = self.release_one()["output"]
        rc, err = self.run_cli("spec", "impl", "x\n", extra=["--inputs", "07-research-to-spec.md"])
        self.assertEqual((1, True), (rc, "[P2]" in err))
        rc, err = self.run_cli("spec", "impl", f"Inputs: {first_name} " + "ab" * 32 + "\n")
        self.assertEqual((1, True), (rc, "[P2]" in err))

    def test_verify_checks_inputs_hash_against_earlier_release(self):
        first = self.release_one()
        self.assertEqual(0, self.run_cli("spec", "impl", f"Inputs: {first['output']} {first['output_sha256']}\n")[0])
        self.assertEqual(0, self.verify()[0])
        # Forge: swap a released file's Inputs hash and fix up its audit/manifest hashes.
        p = os.path.join(self.hand, "02-spec-to-impl.md")
        body = open(p).read().replace(first["output_sha256"], "cd" * 32)
        os.chmod(p, 0o600)
        put(p, body)
        rc, err = self.verify()
        self.assertEqual(1, rc)
        self.assertIn("matches no earlier released entry", err)

    def test_duplicate_content_refused(self):
        self.assertEqual(0, self.run_cli("research", "spec", "facts\n", unique=False)[0])
        rc, err = self.run_cli("spec", "impl", "facts\n", unique=False, name="copy.md")
        self.assertEqual(1, rc)
        self.assertIn("[D1]", err)
        self.assertEqual(["01-research-to-spec.md", "MANIFEST.txt"], self.listing())

    def test_leftover_tmp_blocks_release_and_is_reported(self):
        self.release_one()
        tmp = os.path.join(self.hand, ".tmp-02-spec-to-impl.md")
        put(tmp, "half")
        rc, err = self.run_cli("spec", "impl", "next\n")
        self.assertEqual(2, rc)
        self.assertIn("leftover temp file", err)
        self.assertTrue(os.path.exists(tmp))  # never deleted silently
        rc, err = self.verify()
        self.assertEqual(1, rc)
        self.assertIn("leftover temp file", err)


SHA = "3ddf2a33" + "0123456789abcdef0123456789abcdef"[:32]
GOOD = ("Absent: _FooBar, _BazQux\nBranch: feature/x  Commit: " + SHA +
        "\nCallers exist: yes; files: src/a.c, include/b.h\n")


class ReconTests(CliBase):
    def setUp(self):
        super().setUp()
        self.assertEqual(0, self.run_cli("research", "spec", "t1\n")[0])
        self.assertEqual(0, self.run_cli("spec", "impl", "t2\n")[0])
        self.spec_impl = [r for r in self.audit() if r["to"] == "impl"][0]
        self.hdr = f"Agent: recon-1\nInputs: {self.spec_impl['output']} {self.spec_impl['output_sha256']}\n"

    def recon(self, body, hdr=None, **kw):
        return self.run_cli("recon", "impl", (self.hdr if hdr is None else hdr) + body, unique=False, agent=True, **kw)

    def test_good_file_released_with_audit_flags(self):
        rc, err = self.recon(GOOD)
        self.assertEqual(0, rc, err)
        r = self.audit()[-1]
        self.assertEqual((True, "never", "recon"), (r["stage_tainted_by_reading"], r["reuse"], r["from"]))
        self.assertEqual(0, self.verify()[0])
        self.assertTrue(self.last_out.rstrip().endswith("recon_releases=1"), self.last_out)
        self.assertEqual(0, self.recon("Absent: _A\nBranch: m  Commit: " + SHA + "\nCallers exist: no\n",
                                       hdr=self.hdr + "\n")[0])

    def test_only_recon_to_impl_allowed(self):
        for src, dst in [("research", "recon"), ("spec", "recon"), ("impl", "recon"), ("validate", "recon"),
                         ("recon", "spec"), ("recon", "validate"), ("recon", "research"), ("recon", "recon")]:
            rc, err = self.run_cli(src, dst, "x\n", extra=["--agent-id", "a"])
            self.assertEqual((1, True), (rc, "[X1]" in err), (src, dst))

    def test_f1_failing_variants(self):
        bad = {
            "four lines": GOOD + "Extra: line\n",
            "free text": "The functions are absent now and the callers do not exist.\n",
            "missing field": "Absent: _A\nBranch: m  Commit: " + SHA + "\n",
            "short sha": "Absent: _A\nBranch: m  Commit: 3ddf2a33\nCallers exist: no\n",
            "absolute path": "Absent: _A\nBranch: m  Commit: " + SHA + "\nCallers exist: yes; files: /src/a.c\n",
            "dotdot": "Absent: _A\nBranch: m  Commit: " + SHA + "\nCallers exist: yes; files: a/../b.c\n",
            "system path": "Absent: _A\nBranch: m  Commit: " + SHA + "\nCallers exist: yes; files: System/Library/x.c, /System/Library/x\n",
            "yes without files": "Absent: _A\nBranch: m  Commit: " + SHA + "\nCallers exist: yes\n",
            "no with files": "Absent: _A\nBranch: m  Commit: " + SHA + "\nCallers exist: no; files: a.c\n",
            "names with spaces": "Absent: a name here\nBranch: m  Commit: " + SHA + "\nCallers exist: no\n",
        }
        for label, body in bad.items():
            rc, err = self.recon(body)
            self.assertEqual((1, True), (rc, "[F1]" in err), label)
            self.assertRegex(err, r":\d+: \[F1\]", label)

    def test_inputs_must_cite_spec_to_impl_release(self):
        t1 = [r for r in self.audit() if r["to"] == "spec"][0]
        for hdr in ("Agent: r\n",
                    f"Agent: r\nInputs: {t1['output']} {t1['output_sha256']}\n",
                    f"Agent: r\nInputs: {self.spec_impl['output']} {'0' * 64}\n",
                    f"Agent: r\nInputs: free text\n"):
            rc, err = self.recon(GOOD, hdr=hdr)
            self.assertEqual((1, True), (rc, "[P2]" in err), hdr)

    def test_agent_required_and_flag_disagreement(self):
        p = self.write(self.hdr.replace("Agent: recon-1\n", "") + GOOD, "recon", "n.md")
        rc, err, _ = self.call(["--run-root", self.root, "--from", "recon", "--to", "impl", "--file", p])
        self.assertEqual((1, True), (rc, "[P3]" in err))

    def test_existing_rules_still_apply_inside_recon_file(self):
        for extra in ("00001000  de ad be ef 00 11 22 33  44\n", "mov x0, x1\nldr x2, [sp]\nbl _f\n", "sub_1A2B\n"):
            rc, err = self.recon(GOOD + extra)
            self.assertEqual(1, rc)
            self.assertRegex(err, r"\[(R4|R1|R5|F1)\]")
        rc, err = self.recon("Absent: _A\nBranch: m  Commit: " + SHA + "\nCallers exist: yes; files: /System/Library/x\n")
        self.assertIn("[R6]", err)

    def test_recon_workspace_mode_checked(self):
        os.chmod(os.path.join(self.root, "recon") if os.path.isdir(os.path.join(self.root, "recon"))
                 else self.write("", "recon", "z") and os.path.join(self.root, "recon"), 0o755)
        rc, err = self.recon(GOOD)
        self.assertEqual((2, True), (rc, "group/world" in err))


if __name__ == "__main__":
    unittest.main()
