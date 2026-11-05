#!/usr/bin/env python3
"""Clean-room handoff checker for the missing-symbol workflow.

Tripwire, not proof: it rejects text that is shaped like disassembly,
decompiler output, tool dumps, raw trace lines or Apple image paths, and keeps
an append-only, hash-chained audit trail of every release and rejection.

  check_handoff.py --run-root DIR --from STAGE --to STAGE --file PATH [--agent-id ID] [--inputs A,B]
  check_handoff.py --verify-chain --run-root DIR   (or --summary)
"""
import argparse
import datetime
import fcntl
import hashlib
import json
import os
import re
import stat
import sys
import unicodedata

STAGES = ("research", "spec", "impl", "validate")
FORWARD = {("research", "spec"), ("spec", "impl"), ("impl", "validate"), ("spec", "validate")}
# Back-channels carry behavioural questions or mismatch reports; same checks.
BACK = {("impl", "spec"), ("validate", "impl"), ("validate", "spec")}
ALLOWED = FORWARD | BACK
# Hits on these rules go to coord-compliance (possible exposure to disassembly), not just back to the author.
COMPLIANCE_RULES = {"R1", "R3", "R5"}

# R4: more than this many raw hex words (32- or 64-bit, 8+ hex digits) in a line or adjacent lines is rejected.
MAX_HEX_WORDS = 4
# R1: asm-looking lines in a row, or in one blank-line-delimited block.
ASM_RUN = 3
ASM_BLOCK = 6

GENESIS = "0" * 64
AUDIT = "AUDIT.jsonl"
MANIFEST = "MANIFEST.txt"

# ---- R1 tables --------------------------------------------------------------
# Best-effort: a fixed list of common AArch64/x86 mnemonics, not a disassembler (ARMv7 r0-r7, x87, string ops are not covered).
MNEM_ARM = ("mov movz movk movn ldr ldp ldur ldrb ldrh ldrsw str stp stur strb strh add adds sub subs "
            "adrp adr cmp cmn tst cbz cbnz tbz tbnz bl blr br csel cset orr and eor lsl lsr asr svc "
            "b mul madd ubfx sbfx ldar ldaxr stlr stlxr fmov casal cas ldxr stxr "
            "fadd fsub fmul fdiv fsqrt fabs fneg fcmp fcvt fcvtzs fcvtzu scvtf ucvtf fmadd fmsub fmax fmin fcsel "
            "bic bics orn eon mvn neg negs ngc adc adcs sbc sbcs sxtb sxth sxtw uxtb uxth uxtw clz cls rbit "
            "rev rev16 rev32 sdiv udiv smull umull smulh umulh msub mneg ror extr bfi bfxil bfm ubfm sbfm ubfiz sbfiz "
            "csinc csinv csneg cinc cinv cneg ccmp ccmn ldrsb ldrsh ldnp stnp ldtr sttr ldursw ldurb ldurh sturb sturh "
            "ldpsw ldrsw ldaxp stxp stlxp ldarb ldarh stlrb stlrh ldxrb ldxrh stxrb stxrh ldadd ldclr ldeor ldset swp "
            "ld1 st1 movi dup umov prfm msr mrs dmb dsb isb brk bx blx beq bne blt bgt ble bge bhi bls bcs bcc bmi bpl").split()
_X86_BASE = "add sub and or xor cmp test shl shr sar neg not inc dec imul lea mov adc sbb mul div idiv".split()
_X86_CC = "e ne z nz a ae b be g ge l le s ns o no p np c nc".split()
MNEM_X86 = ("mov movq movl movzx movsx push pop call lea jmp je jne jz jnz ja jb jg jl jle jge "
            "cmp test xor add sub and or imul shl shr inc dec "
            "sar sal rol ror rcl rcr not neg xchg adc sbb mul div idiv movabs movsxd movzbl movzwl movsbl movswl movslq "
            "cmpxchg xadd bt bts btr btc bsf bsr tzcnt lzcnt popcnt bswap pushq popq pushf popf callq jmpq "
            "jae jna jnae jnb jnbe jng jnge jnl jnle jc jnc jo jno js jns jp jnp jcxz jecxz jrcxz loop "
            "movss movsd movaps movups movapd movdqa movdqu pxor xorps xorpd addss addsd subss subsd mulss mulsd divss divsd "
            "ucomiss ucomisd cvtsi2sd cvtsi2ss").split()
MNEM_X86 += [b + x for b in _X86_BASE for x in "bwlq"]
MNEM_X86 += ["set" + c for c in _X86_CC] + ["cmov" + c for c in _X86_CC]
MNEM_BARE = ("ret retq leave nop cltq cqto hlt int3 cdq cqo cwde cdqe cltd cwtl cbtw syscall sysenter ud2 eret wfi wfe").split()
# Only these may take a bare label/address operand ("call _foo").
MNEM_BRANCH = set("bl blr br bx blx call callq jmp jmpq je jne jz jnz ja jb jg jl jle jge "
                  "jae jbe jna jnae jnb jnbe jng jnge jnl jnle jnc jno jns jnp jcxz jecxz jrcxz cbz cbnz".split())
# Short or word-like: a bare label operand only counts if it looks like a symbol or address ("b main" is prose).
MNEM_WEAK_BRANCH = set("b loop js jo jc jp beq bne blt bgt ble bge bhi bls bcs bcc bmi bpl".split())
# Word-like mnemonics: "fix #12" and "neg #14" are prose, so a lone #N or $N immediate does not make them instructions.
MNEM_PROSE_IMM = set("fix rev rev16 rev32 neg negs not swp dup bic orn eon mvn ror cas cls clz sar sal".split())

_REG = (r"%?(?:[xw](?:[0-9]|[12][0-9]|30)|sp|lr|fp|xzr|wzr|[qdsvb][0-9]{1,2}(?:\.\w+)?"
        r"|[re]?(?:ax|bx|cx|dx|si|di|bp|sp|ip)|[abcd][lh]|r(?:[89]|1[0-5])[dwb]?|xmm[0-9]+)")
_IMM = r"(?:[#$]-?(?:0x[0-9a-f]+|[0-9]+)|-?0x[0-9a-f]+|-?[0-9]+)"
_MEM = (r"(?:(?:[a-z]+\s+ptr\s+)?(?:[a-z]s:)?\[[^\]\n]{1,40}\]!?"
        r"|-?(?:0x[0-9a-f]+|[0-9]+)?\(%[a-z0-9]+[^)\n]{0,20}\))")
_LABEL = r"\*?[A-Za-z_.$][\w.$@]*"
_OPS = rf"(?:{_REG}|{_IMM}|{_MEM}|{_LABEL})"
_MNEM = "|".join(sorted(set(MNEM_ARM + MNEM_X86), key=len, reverse=True))
ASM_LINE = re.compile(
    rf"^\s*(?:(?:0x)?[0-9a-f]{{4,16}}:\s*)?(?P<m>{_MNEM}|b\.[a-z]{{2}})(?P<ops>\s+{_OPS}(?:\s*,\s*{_OPS}){{0,3}})"
    r"\s*(?:(?:;|//|@).*)?$", re.I)
BARE_LINE = re.compile(rf"^\s*(?:(?:0x)?[0-9a-f]{{4,16}}:\s*)?(?:{'|'.join(MNEM_BARE)})\s*(?:(?:;|//).*)?$", re.I)
_REGMEM_OP = re.compile(rf"^(?:{_REG}|{_MEM}|-?0x[0-9a-f]+)$", re.I)
_IMM_OP = re.compile(r"^[#$]-?(?:0x[0-9a-f]+|[0-9]+)$", re.I)
_STRICT_OP = re.compile(rf"^(?:{_REG}|{_MEM}|[#$]-?(?:0x[0-9a-f]+|[0-9]+)|-?0x[0-9a-f]+)$", re.I)
_SYMBOL_OP = re.compile(r"[_.:]|^[#$]?-?0x[0-9a-f]+$", re.I)

# Ghidra decompiler names. Strong signatures are decisive alone; weak names (param_1, iVar1, short DAT_) are
# ordinary identifiers and only count when undefinedN and another weak kind share a block.
_NB = r"(?<![A-Za-z0-9])"
GHIDRA_STRONG = (rf"{_NB}(?:FUN|DAT|LAB|UNK)_[0-9A-Fa-f]{{8,}}\b|{_NB}S?CONCAT\d{{2}}\(|{_NB}SUB\d{{2}}\("
                 rf"|{_NB}[SZ]EXT\d{{2,3}}\b|{_NB}in_stack_[0-9A-Fa-f]{{4,}}\b|{_NB}(?:unaff|extraout)_[A-Za-z]\w*"
                 rf"|{_NB}local_res[0-9A-Fa-f]+\b")
GHIDRA_WEAK = [re.compile(x) for x in (
    rf"{_NB}[a-z]{{1,6}}Var\d+\b", rf"{_NB}param_\d+\b", rf"{_NB}local_[0-9A-Fa-f]+\b",
    rf"{_NB}(?:FUN|DAT|LAB|UNK)_[0-9A-Fa-f]{{4,7}}\b", rf"{_NB}undefined[1-8]\b", rf"{_NB}in_[A-Z]{{2,5}}[0-9]?\b")]


GHIDRA_UNDEFINED = 4  # index of the undefinedN pattern in GHIDRA_WEAK


def ghidra_weak_kinds(line):
    return {i for i, rx in enumerate(GHIDRA_WEAK) if rx.search(line)}


def corroborated_ghidra_lines(lines):
    """1-based lines with a weak Ghidra name, in blocks (runs of non-blank lines) that also hold undefinedN."""
    out, block, kinds = [], [], set()
    for i, line in enumerate(lines + [""], 1):
        if line.strip():
            k = ghidra_weak_kinds(line)
            if k:
                block.append(i)
                kinds |= k
            continue
        if GHIDRA_UNDEFINED in kinds and len(kinds) >= 2:
            out += block
        block, kinds = [], set()
    return out


# ---- line-level rule tables: (rule id, description, regex) ------------------
_H = r"[0-9a-fA-F]"
LINE_RULES = [
    ("R2", "address+offset notation", re.compile(
        rf"<\+\d+>|<[\w.$]+\+(?:0x{_H}+|\d+)>|^\s*(?:=>\s*)?0x{_H}{{8,16}}\s*:"
        rf"|\b[A-Za-z_]\w*\s*\+\s*0x{_H}+\b|\b_[A-Za-z]\w*\+\d+\b", re.I)),
    ("R3", "disassembler/dump tool output signature", re.compile(
        r"Contents of \(__\w+,__\w+\)|\(__[A-Z]+,__\w+\)\s+section|file format\s+(?:mach-?o|elf\d*)"
        r"|^\s*cmd\s+LC_\w+|^\s*Idx\s+Name\s+Size\s+VMA|^\s*(?:rebase|lazy bind|weak bind|bind|export) information",
        re.I)),
    ("R4", "hex-dump line", re.compile(
        rf"^\s*(?:0x)?{_H}{{4,16}}:?\s+(?:{_H}{{2}}\s+){{8,}}|^\s*{_H}{{6,16}}:\s+(?:{_H}{{4}}\s+){{4,}}")),
    ("R5", "decompiler pseudocode marker", re.compile(
        r"\b(?:sub|loc)_[0-9A-Fa-f]+\b|\b(?:LOBYTE|HIBYTE|LOWORD|HIWORD|LODWORD|HIDWORD)\s*\("
        r"|(?<![\w.])v\d+\s*=(?!=)|\b__int(?:8|16|32|64)\b|\b_(?:QWORD|DWORD|WORD|BYTE)\b"
        r"|\b__(?:fastcall|cdecl|stdcall|usercall)\b|" + GHIDRA_STRONG)),
    ("R6", "path into Apple system image, install volume or shared cache", re.compile(
        r"/System/(?:Library|Volumes|Cryptexes|Applications)|/Library/Apple\b"
        r"|/Volumes/[^/\s]+/(?:System|Library|usr|Applications)\b"
        r"|dyld_shared_cache|\.dyld\b|/usr/lib/system/\S*\.dylib", re.I)),
    ("R8", "forbidden trace field as key", re.compile(
        r"""["'](?:lr|pc|backtrace|image_offset|return_address)["']\s*:"""
        r"""|(?<![\w.])(?:lr|pc)\s*[:=]\s*0x[0-9a-f]+"""
        r"""|(?<![\w.])(?:backtrace|image_offset|return_address)\s*[:=]\s*(?:0x[0-9a-f]+|\[|\{|\d)""",
        re.I)),
]
CLASSDUMP_IVAR = re.compile(rf";\s*(?://|/\*)\s*(?:\d+\s*=\s*)?[+@]?0x{_H}+")
OBJC_INTERFACE = re.compile(r"^\s*@interface\s+\w+")
HEX_WORD = re.compile(
    rf"(?<![\w])(?:0[xX][0-9a-fA-F_']{{8,24}}|(?:{_H}{{4}}_){{1,3}}{_H}{{4}}|{_H}{{16}}|[0-9]{{15,20}}"
    rf"|(?=[0-9a-fA-F]*[0-9])(?=[0-9a-fA-F]*[a-fA-F])[0-9a-fA-F]{{8,15}})(?![\w])")

# ---- R7 tables ---------------------------------------------------------------
TRACE_SYMBOL_KEYS = {"symbol", "sym", "name"}
TRACE_REG_KEY = re.compile(r"^(?:x[0-7]|rdi|rsi|rdx|rcx|r8|r9)$")
TRACE_STRONG_ARG_KEYS = {"regs", "arg_regs", "fp_regs", "words"}
MAX_JSON_FAILURES = 2000  # fail closed: brace-heavy files are not scanned forever


def is_trace_obj(obj):
    keys = {k.lower() for k in obj if isinstance(k, str)}
    if not keys & TRACE_SYMBOL_KEYS:
        return False
    # "args" alone is too common next to "name" in ordinary JSON examples.
    return bool(keys & TRACE_STRONG_ARG_KEYS or any(TRACE_REG_KEY.match(k) for k in keys)
                or ("args" in keys and keys & {"symbol", "sym"}))


def walk_dicts(obj):
    stack = [obj]
    while stack:
        o = stack.pop()
        if isinstance(o, dict):
            yield o
            stack.extend(o.values())
        elif isinstance(o, list):
            stack.extend(o)


def trace_hits(text):
    """Line numbers of JSON objects (one-line, multi-line, bulleted, inline) that look like T0."""
    dec, hits, fails = json.JSONDecoder(), [], 0
    i = text.find("{")
    while i != -1:
        try:
            obj, end = dec.raw_decode(text, i)
        except (ValueError, RecursionError):
            fails += 1
            if fails > MAX_JSON_FAILURES:
                return hits + [0]
            i = text.find("{", i + 1)
            continue
        if any(is_trace_obj(d) for d in walk_dicts(obj)):
            hits.append(text.count("\n", 0, i) + 1)
        i = text.find("{", end)
    return hits


MD_PREFIX = re.compile(r"^[\s>|]*(?:(?:[-*+\u2022]|\d+[.)])\s+)*")


def strip_md(line):
    """Drop bullets, numbering, table pipes and wrapping backticks so rules see the content."""
    return MD_PREFIX.sub("", line).strip().strip("`|").strip()


def asm_like(line):
    if BARE_LINE.match(line):
        return True
    m = ASM_LINE.match(line)
    if not m:
        return False
    ops = [o.strip() for o in m.group("ops").split(",")]
    mnem = m.group("m").lower()
    if mnem in MNEM_BRANCH:
        return True
    if mnem in MNEM_WEAK_BRANCH:
        return any(_REGMEM_OP.match(o) or _SYMBOL_OP.search(o) for o in ops)
    # Without a register, memory operand, hex address or immediate, "add 5" is prose.
    prose_imm = mnem in MNEM_PROSE_IMM
    return any(_STRICT_OP.match(o) and not re.fullmatch(r"-?[0-9]+", o) and not (prose_imm and _IMM_OP.match(o)) for o in ops)


def asm_weight(line):
    """Instruction lines this line stands for: each instruction-like `;` or `|` segment counts once."""
    segs = [x for x in (p.strip() for p in re.split(r"[;|]", line)) if x]
    if len(segs) >= 2:
        return sum(1 for x in segs if asm_like(x))
    return 1 if asm_like(line) else 0


def check_text(text, src, dst):
    """Return sorted [(line_no, rule_id, description)]. src/dst are kept for the
    signature; every rule applies on every transition."""
    text = unicodedata.normalize("NFKC", text).replace("\ufeff", "").replace("\r\n", "\n")
    lines = [strip_md(l) for l in text.split("\n")]
    hits = []
    has_interface = any(OBJC_INTERFACE.match(l) for l in lines)

    for i, line in enumerate(lines, 1):
        for rid, desc, rx in LINE_RULES:
            if rx.search(line):
                hits.append((i, rid, desc))
        if has_interface and (OBJC_INTERFACE.match(line) or CLASSDUMP_IVAR.search(line)):
            hits.append((i, "R3", "class-dump style @interface with ivar offsets"))
    for i in corroborated_ghidra_lines(lines):
        hits.append((i, "R5", LINE_RULES[3][1]))
    for ln in trace_hits(text):
        hits.append((ln, "R7", "raw trace (T0) JSON object" if ln else "too many JSON candidates to scan"))

    # R1: runs and blocks of operand-shaped instructions.
    block, run = [], []
    for i, line in enumerate(lines + [""], 1):
        if not line.strip():
            if len(block) >= ASM_BLOCK:
                hits.append((block[0], "R1", f"{len(block)} instruction-like lines in one block"))
            block, run = [], []
            continue
        weight = asm_weight(line)
        if weight:
            block += [i] * weight
            run += [i] * weight
            if len(run) >= ASM_RUN > len(run) - weight:
                hits.append((run[0], "R1", f"{ASM_RUN}+ consecutive instruction-like lines"))
        else:
            run = []

    # R4: consecutive 64-bit hex words across adjacent lines; blank lines do not reset the run.
    total, start = 0, 0
    for i, line in enumerate(lines + ["x"], 1):
        n = sum(1 for m in HEX_WORD.finditer(line) if len(re.sub(r"[_']", "", m.group().removeprefix("0x").removeprefix("0X"))) >= 8)
        if n:
            start = start or i
            total += n
            if total > MAX_HEX_WORDS:
                hits.append((start, "R4", f"more than {MAX_HEX_WORDS} consecutive 32/64-bit hex or decimal words"))
                total = -10**9  # report once per run
        elif line.strip():
            total, start = 0, 0
    return sorted(set(hits))


def parse_header(text, label):
    """Value of the first `Label:` line (optional bullet/bold), or None."""
    rx = re.compile(rf"^\s*(?:[*_-]\s*)?\**{label}:\**\s*(.*)$", re.I)
    for line in text.split("\n"):
        m = rx.match(line)
        if m:
            return m.group(1).strip()
    return None


def parse_list(text, label):
    v = parse_header(text, label)
    return [s.strip() for s in v.split(";") if s.strip()] if v else []


def parse_sources(text):
    return parse_list(text, "Sources?")


# ---- audit trail ---------------------------------------------------------------
MAX_INPUT = 1_000_000


def sha256_bytes(b):
    return hashlib.sha256(b).hexdigest()


def now_utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")


class UnsafePath(Exception):
    """Unsafe or unusable path/state; reported on stderr, exit 2."""


def clean_root(run_root):
    root = run_root.rstrip("/") or "/"
    if os.path.islink(root):
        raise UnsafePath(f"run root is a symlink: {run_root}")
    return root


def private_dir(path, create):
    """Return path as an existing 0700-only directory, creating it if allowed."""
    if os.path.islink(path):
        raise UnsafePath(f"refusing symlink: {path}")
    if not os.path.isdir(path):
        if not create:
            raise UnsafePath(f"missing directory: {path}")
        os.mkdir(path, 0o700)
    if stat.S_IMODE(os.stat(path).st_mode) & 0o077:
        raise UnsafePath(f"directory is group/world accessible (chmod 700 it): {path}")
    return path


def prepare_run_root(run_root):
    root = clean_root(run_root)
    if not os.path.isdir(root):
        raise UnsafePath(f"run root does not exist (typo?): {root}")
    private_dir(root, False)
    layout = [s for s in STAGES if os.path.isdir(os.path.join(root, s))]
    if not layout:
        raise UnsafePath(f"run root has no stage workspace dir ({'/ '.join(STAGES)}/): {root}")
    for st in layout:
        private_dir(os.path.join(root, st), False)
    return root


def check_input_location(root, src, path):
    """The input must live in the --from stage workspace."""
    if os.path.islink(path):
        raise UnsafePath(f"input file is a symlink: {path}")
    ws = os.path.join(root, src)
    if not os.path.isdir(ws):
        raise UnsafePath(f"stage workspace missing: {ws}")
    ws, real = os.path.realpath(ws), os.path.realpath(path)
    if os.path.commonpath([ws, real]) != ws:
        raise UnsafePath(f"input file is outside the {src} workspace ({ws}): {path}")


def read_input(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
    try:
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            raise UnsafePath(f"input is not a regular file: {path}")
        data = os.read(fd, MAX_INPUT + 1)
        while len(data) <= MAX_INPUT:
            more = os.read(fd, MAX_INPUT + 1 - len(data))
            if not more:
                break
            data += more
    finally:
        os.close(fd)
    if len(data) > MAX_INPUT:
        raise UnsafePath(f"input larger than {MAX_INPUT} bytes: {path}")
    return data


def open_nofollow(path, flags):
    if os.path.islink(path):
        raise UnsafePath(f"refusing symlink: {path}")
    return os.open(path, flags | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)


def lock_audit(d):
    fd = open_nofollow(os.path.join(d, AUDIT), os.O_RDWR | os.O_APPEND | os.O_CREAT)
    fcntl.flock(fd, fcntl.LOCK_EX)
    os.fchmod(fd, 0o600)
    return fd


def read_records(fd):
    """Raw audit lines; refuses to extend a chain whose lines are not JSON objects."""
    size = os.fstat(fd).st_size
    raw = os.pread(fd, size, 0) if size else b""
    lines = [l for l in raw.split(b"\n") if l]
    for i, l in enumerate(lines, 1):
        try:
            ok = isinstance(json.loads(l), dict)
        except ValueError:
            ok = False
        if not ok:
            raise UnsafePath(f"AUDIT.jsonl line {i} is corrupt; run --verify-chain and investigate")
    return lines


def append_record(fd, lines, record):
    record["prev_hash"] = sha256_bytes(lines[-1]) if lines else GENESIS
    os.write(fd, (json.dumps(record, sort_keys=True) + "\n").encode())


def release(d, record, data, src, dst):
    """Temp file -> audit record + manifest line -> link as NN-*.md under the audit lock.
    Crash-evident, not atomic: a crash leaves a .tmp file or a record without its file, which
    --verify-chain reports."""
    fd = lock_audit(d)
    try:
        lines = read_records(fd)
        n = sum(1 for l in lines if json.loads(l).get("verdict") == "RELEASE") + 1
        if n > 99:
            raise UnsafePath("handoff sequence exhausted (99)")
        prefix = f"{n:02d}-"
        refuse_broken(d, lines)
        name = f"{prefix}{src}-to-{dst}.md"
        mfd = open_nofollow(os.path.join(d, MANIFEST), os.O_WRONLY | os.O_APPEND | os.O_CREAT)
        tmp = os.path.join(d, f".tmp-{name}")
        try:
            tfd = open_nofollow(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL)
            with os.fdopen(tfd, "wb") as f:
                f.write(data)
            record["output"] = name
            append_record(fd, lines, record)
            os.write(mfd, f"{name} {record['output_sha256']} {src} {';'.join(record['sources'])}\n".encode())
            os.link(tmp, os.path.join(d, name))
        finally:
            os.close(mfd)
            try:
                os.unlink(tmp)
            except FileNotFoundError:
                pass
    finally:
        os.close(fd)


HASH64 = re.compile(r"\b[0-9a-f]{64}\b", re.I)


def released_map(lines):
    """name -> (output_sha256, to-stage) for every RELEASE record."""
    out = {}
    for l in lines:
        r = json.loads(l)
        if r.get("verdict") == "RELEASE":
            out[r["output"]] = (r["output_sha256"], r["to"])
    return out


def refuse_broken(d, lines):
    """A crashed release must be investigated, not papered over with more records."""
    released = released_map(lines)
    names = os.listdir(d)
    for n in sorted(names):
        if n.startswith(".tmp-"):
            raise UnsafePath(f"leftover temp file {n} from a crashed release; "
                             "run --verify-chain, investigate and remove it by hand")
    for n in sorted(released):
        if n not in names:
            raise UnsafePath(f"released file {n} is missing (crashed release?); "
                             "run --verify-chain and investigate")
    for n in sorted(names):
        if re.match(r"^\d\d-", n) and n not in released:
            raise UnsafePath(f"stray file {n} has no audit record; run --verify-chain and investigate")


def manifest_hashes(d):
    p = os.path.join(d, MANIFEST)
    if os.path.islink(p):
        raise UnsafePath(f"refusing symlink: {p}")
    if not os.path.isfile(p):
        return set()
    with open(p) as f:
        return {x.split()[1] for x in f if len(x.split()) >= 2}


# Accepted Inputs: header form from spec onward: "<NN-from-to-to.md> <sha256>" entries separated by ; or ,
INPUT_ENTRY = re.compile(r"(\d\d-[a-z]+-to-[a-z]+\.md)\s+([0-9a-fA-F]{64})")
NEEDS_AGENT = ("impl", "validate")


def provenance(text, released, src, agent_flag, inputs_flag):
    """Return (agent, input names, input hashes, reasons) after cross-checking flags, header and chain."""
    reasons = []

    def resolve(name, digest=None):
        got = released.get(name)
        if not got or got[1] != src or (digest and digest.lower() != got[0]):
            reasons.append((0, "P2", "input is not an earlier entry released to this stage "
                                     "(name and sha256 must match a release whose 'to' is --from)"))
            return False
        return True

    h_agent = parse_header(text, "Agent")
    if agent_flag and h_agent and agent_flag != h_agent:
        reasons.append((0, "P1", "--agent-id disagrees with the Agent: header"))
    agent = agent_flag or h_agent
    if src in NEEDS_AGENT and not agent:
        reasons.append((0, "P3", f"no agent id (--agent-id or Agent: header) for stage {src}"))

    flag_names = [n.strip() for n in inputs_flag.split(",") if n.strip()] if inputs_flag else []
    for n in flag_names:
        resolve(n)
    names = list(flag_names)
    h_raw = parse_header(text, "Inputs")
    if h_raw is not None and src != "research":
        entries = [e.strip() for e in re.split(r"[;,]", h_raw) if e.strip()]
        h_names = []
        for e in entries:
            m = INPUT_ENTRY.fullmatch(e)
            if not m:
                reasons.append((0, "P2", "Inputs: entry not in the form '<NN-from-to-to.md> <sha256>'"))
            elif resolve(m.group(1), m.group(2)):
                h_names.append(m.group(1))
        if flag_names and set(flag_names) != set(h_names):
            reasons.append((0, "P1", "--inputs disagrees with the Inputs: header"))
        names = names or h_names
    elif h_raw is not None:
        names = names or parse_list(text, "Inputs")
    hashes = [released[n][0] for n in names if n in released]
    return agent, names, hashes, reasons


def run_check(args):
    src, dst = args.from_stage, args.to_stage
    root = prepare_run_root(args.run_root)
    check_input_location(root, src, args.file)
    data = read_input(args.file)
    d = private_dir(os.path.join(root, "handoff"), True)

    reasons = []
    if (src, dst) not in ALLOWED:
        reasons.append((0, "X1", f"transition {src}->{dst} not allowed"))
    if b"\0" in data:
        reasons.append((0, "R9", "binary content (NUL byte)"))
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        text = data.decode("utf-8", "replace")
        reasons.append((0, "R9", "input is not valid UTF-8"))
    reasons += check_text(text, src, dst)
    in_hash = sha256_bytes(data)
    if in_hash in manifest_hashes(d):
        reasons.append((0, "D1", "identical content is already released (re-release of a copy)"))

    fd = lock_audit(d)
    try:
        lines = read_records(fd)
        refuse_broken(d, lines)
        agent, names, hashes, prov = provenance(text, released_map(lines), src, args.agent_id, args.inputs)
        reasons += prov
        rec = {"time": now_utc(), "from": src, "to": dst, "author": src, "agent": agent,
               "inputs": names, "input_hashes": hashes, "input_sha256": in_hash,
               "output_sha256": None, "sources": parse_sources(text),
               "report_to_compliance": False}
        if reasons:
            ids = sorted({r[1] for r in reasons})
            rec.update(verdict="REJECT", rules=ids, output=None,
                       report_to_compliance=bool(COMPLIANCE_RULES & set(ids)))
            append_record(fd, lines, rec)
    finally:
        os.close(fd)
    if reasons:
        # Reasons carry rule text only, never the matched content.
        for ln, rid, desc in reasons:
            print(f"{args.file}:{ln}: [{rid}] {desc}", file=sys.stderr)
        for rid in sorted(COMPLIANCE_RULES & {r[1] for r in reasons}):
            print(f"REPORT-TO-COMPLIANCE: rule {rid}", file=sys.stderr)
        return 1
    rec.update(verdict="RELEASE", rules=[], output_sha256=in_hash)
    release(d, rec, data, src, dst)
    print(f"released {os.path.join(d, rec['output'])}")
    return 0


TAINT_DIR = re.compile(r"^(?:research|spec|impl|validate)\.tainted$")


def count_tainted(root):
    """Per-agent subdirs of <stage>.tainted dirs under the run root; also returns bare dirs (no subdir)."""
    n, bare = 0, []
    for name in sorted(os.listdir(root)):
        p = os.path.join(root, name)
        if TAINT_DIR.match(name) and os.path.isdir(p) and not os.path.islink(p):
            k = sum(1 for c in os.listdir(p) if os.path.isdir(os.path.join(p, c)))
            n += k
            if not k:
                bare.append(name)
    return n, bare


def summary_line(root, lines, prev):
    tainted, bare = count_tainted(root)
    for b in bare:
        print(f"warning: {b} has no <agent-id> subdirectory and counts 0; "
              f"taints belong in {b}/<agent-id>/", file=sys.stderr)
    flagged = 0
    for l in lines:
        try:
            flagged += bool(json.loads(l).get("report_to_compliance"))
        except (ValueError, AttributeError):
            pass
    return f"records={len(lines)} last_hash={prev} tainted_stages={tainted} compliance_reports={flagged}"


def verify_chain(run_root, check=True):
    root = clean_root(run_root)
    d = os.path.join(root, "handoff")
    problems = []
    if os.path.islink(d) or not os.path.isdir(d):
        print("handoff dir missing or a symlink", file=sys.stderr)
        return 1
    audit = os.path.join(d, AUDIT)
    if os.path.islink(audit) or not os.path.isfile(audit):
        print("AUDIT.jsonl missing or a symlink", file=sys.stderr)
        return 1
    with open(audit, "rb") as f:
        lines = [l for l in f.read().split(b"\n") if l]
    prev = sha256_bytes(lines[-1]) if lines else GENESIS
    if not check:
        print(summary_line(root, lines, prev))
        return 0
    prev, released = GENESIS, {}
    for i, raw in enumerate(lines, 1):
        try:
            rec = json.loads(raw)
            if not isinstance(rec, dict):
                raise ValueError
        except ValueError:
            problems.append(f"audit line {i}: not a JSON object")
            break
        if rec.get("prev_hash") != prev:
            problems.append(f"audit line {i}: prev_hash mismatch (record removed or altered before it)")
        prev = sha256_bytes(raw)
        if rec.get("verdict") == "RELEASE":
            name = rec.get("output", "")
            path = os.path.join(d, name)
            if os.path.islink(path) or not os.path.isfile(path):
                problems.append(f"audit line {i}: released file {name} missing")
            else:
                with open(path, "rb") as f:
                    body = f.read()
                if sha256_bytes(body) != rec.get("output_sha256"):
                    problems.append(f"audit line {i}: released file {name} was modified")
                inputs_hdr = parse_header(body.decode("utf-8", "replace"), "Inputs") or ""
                for h in HASH64.findall(inputs_hdr) if rec.get("from") != "research" else []:
                    if (h.lower(), rec.get("from")) not in released.values():
                        problems.append(f"{name}: Inputs hash {h[:12]}... matches no earlier released entry")
            released[name] = (rec.get("output_sha256"), rec.get("to"))
    for name in sorted(os.listdir(d)):
        if re.match(r"^\d\d-.*\.md$", name) and name not in released:
            problems.append(f"{name}: released file with no audit record")
        if name.startswith(".tmp-"):
            problems.append(f"{name}: leftover temp file from a crashed release")
    mpath = os.path.join(d, MANIFEST)
    manifest = {}
    if os.path.islink(mpath):
        problems.append("MANIFEST.txt is a symlink")
    elif os.path.isfile(mpath):
        with open(mpath) as f:
            for l in f:
                p = l.split()
                if len(p) >= 2:
                    manifest[p[0]] = p[1]
    if manifest != {n: h for n, (h, _) in released.items()}:
        problems.append("MANIFEST.txt does not match released records in AUDIT.jsonl")
    for p in problems:
        print(p, file=sys.stderr)
    if problems:
        return 1
    print(summary_line(root, lines, prev))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run-root", required=True)
    ap.add_argument("--from", dest="from_stage", choices=STAGES)
    ap.add_argument("--to", dest="to_stage", choices=STAGES)
    ap.add_argument("--file")
    ap.add_argument("--agent-id", help="author agent id, written to the audit record")
    ap.add_argument("--inputs", help="comma-separated released file names this file was built from")
    ap.add_argument("--verify-chain", action="store_true")
    ap.add_argument("--summary", action="store_true",
                    help="print the summary line without checking the chain. tainted_stages counts "
                         "the immediate subdirs (one per agent id) of <run-root>/{research,spec,impl,validate}.tainted")
    a = ap.parse_args(argv)
    try:
        if a.verify_chain or a.summary:
            return verify_chain(a.run_root, check=a.verify_chain)
        if not (a.from_stage and a.to_stage and a.file):
            ap.error("--from, --to and --file are required unless --verify-chain or --summary")
        return run_check(a)
    except (UnsafePath, OSError, ValueError, RecursionError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
