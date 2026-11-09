#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stdio MCP server that lets an AI client inspect and drive the Darling Launcher's own state.

JSON-RPC 2.0 over stdin/stdout, one message per line (MCP stdio transport). Standard library only.
Run it as `python3 -I tools/launcher-mcp.py`. Stdout carries the protocol; all logging goes to stderr.

Environment variables read (nothing else is trusted):
  DARLING_LAUNCHER_DATA_DIR            launcher data root, default ~/.darling-launcher
  DARLING_LAUNCHER_CONFIG              launcher settings file, default <XDG_CONFIG_HOME or ~/.config>/cristim/darling-launcher.conf
  DARLING_LAUNCHER_MCP_ALLOW_ACTIONS   "1" enables launch_app and stop_prefix (still needs confirm=true per call)
"""
import csv
import json
import os
from pathlib import Path
import re
import secrets
import shlex
import stat
import subprocess
import sys
import tempfile
import time
import unicodedata

SERVER_NAME = "darling-launcher-mcp"
SERVER_VERSION = "0.1.0"
LATEST_PROTOCOL = "2025-06-18"
SUPPORTED_PROTOCOLS = (LATEST_PROTOCOL, "2025-03-26", "2024-11-05")

MAX_LINE_BYTES = 256 * 1024
MAX_RESULT_CHARS = 100_000
MAX_CHILD_OUTPUT = 64 * 1024
MAX_CATALOG_BYTES = 4 * 1024 * 1024
MAX_SETTINGS_BYTES = 1024 * 1024
MAX_LOG_LINE_CHARS = 2000
MAX_LOG_LINES = 200
MAX_LOG_READ_BYTES = 512 * 1024
MAX_LOG_TOTAL_CHARS = 50_000
FINDMNT_TIMEOUT = 10.0
SHUTDOWN_TIMEOUT = 60.0
MAX_TRACKED_CHILDREN = 32
SAFE_ENV_NAMES = ("PATH", "HOME", "USER", "LOGNAME", "LANG", "DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY", "XDG_RUNTIME_DIR")

NAME_RE = re.compile(r"^(?!.*\.\.)[A-Za-z0-9][A-Za-z0-9._ +-]{0,127}$")
# Mirrors LauncherReleases::validTag in src/releases.cpp.
TAG_RE = re.compile(r"^v(\d{4})\.(\d\d)\.(\d\d)-([0-9a-f]{7})(?:-r(\d+))?$")
DEVICE_RE = re.compile(r"^/dev/[A-Za-z0-9_/-]+$")
MOUNT_PLAN_ARGUMENTS = {
    "batch": {"mount_root"},
    "cryptex": {"mount_root", "device"},
    "partition": {"device", "filesystem", "backend", "volume_index", "directory"},
    "unmount": {"directory"},
}

LOG_SOURCES = ("launcher", "mcp-launch")
# One file per launch_app call, so output of other running apps is never attributed to a launch.
LAUNCH_LOG_DIR = "mcp-launch"
LAUNCH_LOG_RE = re.compile(r"^\d{8}T\d{6}-[0-9a-f]{8}\.log$")
MAX_LAUNCH_LOGS = 20
MIN_PRUNE_AGE_SECONDS = 3600  # a log this server wrote less than an hour ago may belong to a running app


def log(message):
    print(f"[{SERVER_NAME}] {message}", file=sys.stderr, flush=True)


class ToolError(Exception):
    """A tool-level failure: returned to the client as an isError result with this message."""


class ConfigError(Exception):
    pass


class Config:
    def __init__(self, home, data_dir, config_file, allow_actions):
        self.home = Path(home)
        self.data_dir = Path(data_dir)
        self.config_file = Path(config_file)
        self.allow_actions = allow_actions

    @property
    def prefix_root(self):
        return self.data_dir / "prefixes"

    @property
    def mount_root(self):
        return self.data_dir / "mounts"

    @property
    def runtimes_root(self):
        return self.data_dir / "runtimes"

    @property
    def log_dir(self):
        return self.data_dir / "logs"

    @classmethod
    def from_environ(cls, environ):
        def absolute(name, value):
            if "\0" in value or not value.startswith("/") or os.path.normpath(value) != (value.rstrip("/") or "/"):
                raise ConfigError(f"{name} must be a normalized absolute path, got {value!r}")
            return value.rstrip("/") or "/"

        home = environ.get("HOME", "")
        if not home:
            raise ConfigError("HOME must be set")
        home = absolute("HOME", home)
        data_dir = absolute("DARLING_LAUNCHER_DATA_DIR", environ["DARLING_LAUNCHER_DATA_DIR"]) if environ.get("DARLING_LAUNCHER_DATA_DIR") else home + "/.darling-launcher"
        if environ.get("DARLING_LAUNCHER_CONFIG"):
            config_file = absolute("DARLING_LAUNCHER_CONFIG", environ["DARLING_LAUNCHER_CONFIG"])
        else:
            base = absolute("XDG_CONFIG_HOME", environ["XDG_CONFIG_HOME"]) if environ.get("XDG_CONFIG_HOME") else home + "/.config"
            config_file = base + "/cristim/darling-launcher.conf"
        flag = environ.get("DARLING_LAUNCHER_MCP_ALLOW_ACTIONS", "")
        if flag not in ("", "0", "1"):
            raise ConfigError("DARLING_LAUNCHER_MCP_ALLOW_ACTIONS must be 0 or 1 when set")
        return cls(home, data_dir, config_file, flag == "1")


class Redactor:
    """Removes the home directory, the user-name component of /home/<name>, /Users/<name> and /run/media/<name> paths, and token-shaped
    strings from everything sent to the client. A bare word is never taken for a user name (user "dev" vs /dev/sda1)."""

    TOKENS = [
        re.compile(r"gh[pousr]_[A-Za-z0-9]{20,}"),
        re.compile(r"github_pat_[A-Za-z0-9_]{20,}"),
        re.compile(r"sk-[A-Za-z0-9_-]{20,}"),
        re.compile(r"AKIA[0-9A-Z]{16}"),
        # The lookbehind makes the scan linear: without it every "eyJ" inside a long eyJeyJeyJ... run restarts the greedy class.
        re.compile(r"(?<![A-Za-z0-9_-])eyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{5,}"),
    ]
    SCHEME = re.compile(r"(?i)\b(?:bearer|basic)\s+(?P<v>(?!<redacted>)[^\s\"']+)")
    LABELS = r"(?:api[_-]?key|access[_-]?key|token|secret|password|passwd|authorization)"
    # label, optional quote, = or :, then the whole value (a quoted string or up to whitespace/quote)
    LABELLED = re.compile(r"""(?i)\b""" + LABELS + r"""\b["']?\s*[=:]\s*(?P<v>(?!<redacted>)(?:"[^"]*"|'[^']*'|"[^"\r\n]*|'[^'\r\n]*|[^\s"']+))""")
    # label separated by whitespace only: the next word if it is long enough to be a secret rather than prose ("the password field")
    SPACED = re.compile(r"(?i)\b" + LABELS + r"\b\s+(?P<v>(?!<redacted>)[^\s\"'=:]{6,})")
    # scheme://user:password@host is found by userinfo_spans (a linear scan, not a regex), which has no length limits.
    TOKEN = re.compile(r"[^\s\"']+")
    USER_PART = re.compile(r"[^/@\[\]]*")
    PORT = re.compile(r"\d{1,5}(?:[/\s\"']|$)")
    # The name ends at a delimiter, so PATH=/home/a/bin:/usr/bin and (/home/a) keep their separators.
    GENERIC_HOME = re.compile(r"(?i)/(?:home|Users|run/media)/(?P<v>[^/\s\"'`:;,()\[\]{}<>|&=*?\\]+)")
    MAX_DECODE_DEPTH = 5
    # Hard cap on work per call, so no input (a guest app's output, a settings value) can stall the single-threaded server.
    # Text longer than MAX_SCAN_CHARS + LOOKAHEAD_CHARS is cut to that window, the WHOLE window is redacted, and only then is
    # the redacted result limited to the first MAX_SCAN_CHARS characters of the original: a span that starts before that limit
    # is emitted whole (as its replacement), so a secret straddling the limit never shows a partial prefix, and the raw text
    # after the last span is trimmed back to a token boundary so no partial token is emitted. Never redacted in chunks: a secret
    # that spans whitespace (password hunter2, "a b" values) would be cut in two and leak.
    MAX_SCAN_CHARS = 65536
    LOOKAHEAD_CHARS = 4096
    CUT_DELIMITERS = " \t\r\n\"'"  # token boundaries: the token that crosses the limit is dropped whole, never cut at an inner delimiter
    TRUNCATION_MARKER = "[truncated before redaction]"

    def __init__(self, home):
        homes = {str(home)}
        try:
            homes.add(os.path.realpath(home))
        except OSError:
            pass
        self.homes = [re.compile(re.escape(h) + r"(?![A-Za-z0-9_.-])", re.IGNORECASE) for h in sorted(homes, key=len, reverse=True) if h not in ("", "/")]

    def spans(self, text, paths, secret):
        """(start, end, replacement) for everything to hide in text."""
        found = [(m.start(), m.end(), secret) for pattern in self.TOKENS for m in pattern.finditer(text)]
        found += [m.span("v") + (secret,) for pattern in (self.SCHEME, self.LABELLED, self.SPACED) for m in pattern.finditer(text)]
        found += [span + (secret,) for span in self.userinfo_spans(text)]
        if paths:
            found += [m.span() + ("~",) for pattern in self.homes for m in pattern.finditer(text)]
            found += [m.span("v") + ("<user>",) for m in self.GENERIC_HOME.finditer(text)]
        return found

    @classmethod
    def userinfo_spans(cls, text):
        """Spans of scheme://user:password@host credentials, one per whitespace/quote-delimited token. The password runs to the LAST
        '@' of the token (so it may contain '@' and '/', and nothing of it leaks) and has no length limit. The user part may
        not contain '/', '@' or brackets, so IPv6 hosts and ssh://git@host:22/... are not credentials, and host:8080/path is a
        port, not a password. Linear: per token one rfind, and each '://' scans at most to the next one."""
        if "@" not in text or "://" not in text:
            return []
        spans = []
        for token in cls.TOKEN.finditer(text):
            lo, hi = token.span()
            at = text.rfind("@", lo, hi)
            if at < 0:
                continue
            scheme = text.find("://", lo, at)
            while scheme != -1:
                start = scheme + 3
                colon = text.find(":", start, at)
                if colon != -1 and cls.USER_PART.fullmatch(text, start, colon) and not cls.PORT.match(text, colon + 1):
                    spans.append((start, at))
                    break
                scheme = text.find("://", scheme + 1, at)
        return spans

    # Escapes that decode_once rewrites, and any non-ASCII character (for NFKC folding); everything else is copied in bulk.
    SPECIAL = re.compile(r"%[0-9A-Fa-f]{2}|\\/|\\u00[0-7][0-9A-Fa-f]|[^\x00-\x7f]")

    @classmethod
    def decode_once(cls, text, origin):
        """One decoding layer: ASCII %XX, JSON escapes (\\/ and \\u00XX), then Unicode compatibility folding (fullwidth slashes and
        letters) character by character. origin[i] is the (start, end) in the original string of character i of text.
        Plain runs are copied as slices, so a layer costs time in proportion to the escapes it contains."""
        out, out_origin, position = [], [], 0
        for match in cls.SPECIAL.finditer(text):
            start, end = match.span()
            if start > position:
                out.append(text[position:start])
                out_origin.extend(origin[position:start])
            token = match.group()
            if token[0] == "%" or token[0] == "\\":
                code = ord("/") if token == "\\/" else int(token[-2:], 16)
                decoded = chr(code) if code < 128 else token
                out.append(decoded)
                out_origin.extend([(origin[start][0], origin[end - 1][1])] if code < 128 else origin[start:end])
            else:
                folded = unicodedata.normalize("NFKC", token)
                out.append(folded)
                out_origin.extend([origin[start]] * len(folded))
            position = end
        out.append(text[position:])
        out_origin.extend(origin[position:])
        return "".join(out), out_origin

    def text(self, value, paths=True):
        """Hides secrets and, if paths, home and user-name paths. The text is also searched percent-, JSON-escape- and NFKC-decoded to a fixed point
        (at most MAX_DECODE_DEPTH layers) but the ORIGINAL text is what is returned, with every match redacted whole, so a
        decoded fragment never appears in the output. paths=False keeps paths and uses a replacement without shell
        metacharacters, for words that are quoted afterwards: redaction must never run over an already quoted string."""
        truncated = len(value) > self.MAX_SCAN_CHARS + self.LOOKAHEAD_CHARS
        if truncated:
            value = value[:self.MAX_SCAN_CHARS + self.LOOKAHEAD_CHARS]
        secret = "<redacted>" if paths else "REDACTED"
        current, origin = value, [(i, i + 1) for i in range(len(value))]
        found = self.spans(current, paths, secret)
        for _ in range(self.MAX_DECODE_DEPTH):
            if current.isascii() and "%" not in current and "\\" not in current:
                break
            decoded, decoded_origin = self.decode_once(current, origin)
            if decoded == current:
                break
            current, origin = decoded, decoded_origin
            found += [(origin[a][0], origin[b - 1][1], repl) for a, b, repl in self.spans(current, paths, secret) if b > a]
        if not found and not truncated:
            return value
        found.sort(key=lambda f: (f[0], -f[1]))
        merged = []
        for start, end, repl in found:
            last = merged[-1] if merged else None
            if last and start < last[1]:
                if repl == secret or last[2] == secret:
                    last[1], last[2] = max(last[1], end), secret  # a secret always widens to cover everything it touches
                elif end > last[1]:
                    merged.append([last[1], end, repl])  # paths: no union, the later span keeps only its own remainder
            else:
                merged.append([start, end, repl])
        limit = self.MAX_SCAN_CHARS if truncated else len(value)
        result, position = [], 0
        for start, end, repl in merged:
            if start >= limit:
                break
            result += [value[position:start], repl]
            position = end
        if position < limit:
            tail = value[position:limit]
            if truncated:
                tail = tail[:max(tail.rfind(c) for c in self.CUT_DELIMITERS) + 1]  # no boundary at all: the whole tail run is dropped
            result.append(tail)
        return "".join(result) + (self.TRUNCATION_MARKER if truncated else "")

    def obj(self, value, exempt=()):
        """Redacts every string except the values of dict keys named in exempt, which the tool has already made safe."""
        if isinstance(value, str):
            return self.text(value)
        if isinstance(value, list):
            return [self.obj(v, exempt) for v in value]
        if isinstance(value, dict):
            return {k: v if k in exempt else self.obj(v, exempt) for k, v in value.items()}
        return value


def control_free(value):
    return not any(ord(c) < 32 or ord(c) == 127 for c in value)


def lstat_or_none(path):
    try:
        return os.lstat(path)
    except FileNotFoundError:
        return None
    except OSError as error:
        raise ToolError(f"cannot inspect {path}: {error.strerror}")


def check_no_symlinks(base, relative_parts):
    """Walks base/<parts> and refuses any existing symlink component, so a link cannot redirect a path out of base."""
    current = Path(base)
    for part in relative_parts:
        current = current / part
        info = lstat_or_none(current)
        if info is None:
            return
        if stat.S_ISLNK(info.st_mode):
            raise ToolError(f"refusing symlink at {current}")


def read_regular_file(path, limit, what, follow_symlink=False):
    """Reads a regular file, refusing a final symlink unless allowed and anything over limit bytes."""
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | (0 if follow_symlink else os.O_NOFOLLOW))
    except FileNotFoundError:
        raise ToolError(f"{what} not found")
    except OSError as error:
        raise ToolError(f"cannot open {what}: {error.strerror}")
    with os.fdopen(fd, "rb") as handle:
        info = os.fstat(handle.fileno())
        if not stat.S_ISREG(info.st_mode):
            raise ToolError(f"{what} is not a regular file")
        if info.st_size > limit:
            raise ToolError(f"{what} is larger than {limit} bytes")
        return handle.read(limit + 1)


def read_settings(config):
    """Reads the keys the GUI stores through QSettings (INI, [General] or no section)."""
    keys = ("prefix", "darling", "runtimeRoot", "volume", "knownPrefixes")
    try:
        raw = read_regular_file(config.config_file, MAX_SETTINGS_BYTES, "launcher settings file", follow_symlink=True)
    except ToolError as error:
        if "not found" in str(error):
            return {}
        raise
    result, section = {}, "General"
    for line in raw.decode("utf-8", "replace").splitlines():
        line = line.strip()
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
        elif section == "General" and "=" in line and not line.startswith((";", "#")):
            key, value = line.split("=", 1)
            if key.strip() in keys:
                result[key.strip()] = value.strip()
    for key in keys:
        value = result.get(key, "")
        if len(value) >= 2 and value[0] == value[-1] == '"':
            result[key] = value[1:-1]
    if "knownPrefixes" in result:
        try:
            result["knownPrefixes"] = [p.strip() for p in next(csv.reader([result["knownPrefixes"]]), []) if p.strip()]
        except csv.Error:
            result["knownPrefixes"] = []
    return result


def resolve_prefix(config, value):
    """Returns the prefix directory for a bare name or an absolute path, which must be a direct,
    non-symlink child of the launcher's prefix root (the same set the GUI's prefix selector lists)."""
    if not isinstance(value, str) or not 1 <= len(value) <= 1024:
        raise ToolError("prefix must be a string of 1 to 1024 characters")
    if not control_free(value):
        raise ToolError("prefix must not contain control characters")
    root = config.prefix_root
    try:
        root_real = Path(os.path.realpath(root, strict=True))
    except OSError:
        raise ToolError("the launcher prefix root does not exist; create a prefix in the launcher first")
    if "/" in value:
        if not value.startswith("/"):
            raise ToolError("prefix must be a bare name or an absolute path inside the launcher prefix root")
        parts = value.split("/")[1:]
        if "." in parts or ".." in parts or "" in parts[:-1]:
            raise ToolError("prefix path must not contain '.', '..' or empty components")
        parent, name = "/" + "/".join(parts[:-1]), parts[-1]
        if parent not in (str(root), str(root_real)):
            raise ToolError("prefix path is outside the launcher prefix root; only direct children of the prefix root are accepted")
    else:
        name = value
    if not NAME_RE.match(name):
        raise ToolError("invalid prefix name: use 1 to 128 letters, digits, space or . _ + - starting with a letter or digit, without '..'")
    path = root_real / name
    info = lstat_or_none(path)
    if info is None:
        raise ToolError(f"prefix {name!r} does not exist under the prefix root")
    if stat.S_ISLNK(info.st_mode):
        raise ToolError("refusing prefix that is a symlink")
    if not stat.S_ISDIR(info.st_mode):
        raise ToolError("prefix is not a directory")
    return path


def resolve_under(root, value, label, allow_root):
    """An absolute path equal to or below root, without '.', '..' or symlink components (root itself may be a link)."""
    if not isinstance(value, str) or not 1 <= len(value) <= 1024 or not control_free(value) or not value.startswith("/"):
        raise ToolError(f"{label} must be an absolute path of at most 1024 characters")
    if any(part in ("", ".", "..") for part in value.split("/")[1:]):
        raise ToolError(f"{label} must not contain '.', '..' or empty components")
    root_real = os.path.realpath(root)
    for base in dict.fromkeys((str(root), root_real)):
        if value == base or value.startswith(base + "/"):
            relative = [part for part in value[len(base):].split("/") if part]
            break
    else:
        raise ToolError(f"{label} is outside the launcher mount root {root}")
    if not relative and not allow_root:
        raise ToolError(f"{label} must be a directory below the mount root, not the root itself")
    check_no_symlinks(root_real, relative)
    return Path(root_real).joinpath(*relative)


TRUSTED_DIRECTORIES = ("/usr/local/bin", "/usr/bin")
LSTAT = os.lstat  # replaced by the tests, which cannot create root-owned files


def check_root_chain(path):
    """The file and every ancestor must be owned by root and not group- or world-writable (symlink modes are ignored)."""
    current = Path("/")
    for part in [""] + list(Path(path).parts[1:]):
        current = current / part
        info = LSTAT(current)
        if info.st_uid != 0 or (not stat.S_ISLNK(info.st_mode) and info.st_mode & (stat.S_IWGRP | stat.S_IWOTH)):
            raise ToolError(f"{current} must be owned by root and not group- or world-writable")


# Where `cmake --install` puts mount-macos.py and mount-cryptex.py: <libexecdir>/darling-launcher for the /usr/local and /usr
# prefixes. Mirrors CMakeLists.txt on branch feat/cryptex-mount-action; nothing is imported from there.
INSTALLED_SCRIPT_DIRECTORIES = ("/usr/local/libexec/darling-launcher", "/usr/libexec/darling-launcher")


def trusted_file(directories, name, executable):
    """The resolved path of name in the first of directories that has it, after the root-ownership check of the file and every
    parent both as spelled and as resolved; None if no directory has it."""
    for directory in directories:
        candidate = os.path.join(directory, name)
        if os.path.lexists(candidate):
            resolved = os.path.realpath(candidate)
            check_root_chain(os.path.normpath(candidate))
            check_root_chain(resolved)
            if not stat.S_ISREG(os.stat(resolved).st_mode) or (executable and not os.access(resolved, os.X_OK)):
                raise ToolError(f"{resolved} is not {'an executable ' if executable else 'a regular '}file")
            return resolved
    return None


def system_tool(name):
    """Finds a helper only in fixed root-owned system directories. A path supplied by the client, or a binary a guest
    could have written, never ends up in a command the user is told to run as root. Mirrors tools/mount-cryptex.py."""
    found = trusted_file(TRUSTED_DIRECTORIES, name, True)
    if found is None:
        raise ToolError(f"{name} was not found in {' or '.join(TRUSTED_DIRECTORIES)}; install it there as root")
    return found


USER_PATH_COMPONENT = re.compile(r"(?i)/(home|Users|run/media)/[^/]+")


def home_expansions(home):
    """Shell expressions for the home directory, longest spelling first: "$HOME" and, when HOME is a symlink, its resolved form."""
    home = str(home)
    result = [('"$HOME"', home)]
    resolved = os.path.realpath(home)
    if resolved != home:
        result.append(('"$(realpath -- "$HOME")"', resolved))
    return sorted((r for r in result if r[1] != "/"), key=lambda r: -len(r[1]))


def shell_word(arg, homes):
    """One argv element as a shell word that runs when pasted but names neither the home directory nor a user.
    Under the home directory it becomes "$HOME" plus the quoted rest; a /home/<name>-style component elsewhere
    becomes the quoted placeholder '<user>' (never bare, which a shell would parse as redirection)."""
    for expression, base in homes:
        if arg == base or arg.startswith(base + "/"):
            rest = arg[len(base):]
            return expression + (shlex.quote(rest) if rest else "")
    word, last = "", 0
    for match in USER_PATH_COMPONENT.finditer(arg):
        word += shlex.quote(arg[last:match.start()] + "/" + match.group(1) + "/") + "'<user>'"
        last = match.end()
    return word + (shlex.quote(arg[last:]) if arg[last:] else "") if last else shlex.quote(arg)


def word_value(word, homes):
    """What a POSIX shell (HOME set) makes of one word built by shell_word, without running anything."""
    for expression, base in homes:
        if word.startswith(expression):
            tail = shlex.split("x" + word[len(expression):])
            return base + tail[0][1:] if len(tail) == 1 else None
    parts = shlex.split(word)
    return parts[0] if len(parts) == 1 else None


def check_shell_word(arg, word, homes):
    """Invariant of the builder: the word must expand to exactly arg, or arg with its user-name component hidden."""
    under_home = any(arg == base or arg.startswith(base + "/") for _, base in homes)
    expected = arg if under_home else USER_PATH_COMPONENT.sub(lambda m: f"/{m.group(1)}/<user>", arg)
    try:
        value = word_value(word, homes)
    except ValueError:
        value = None
    if value != expected:
        raise ToolError("cannot build a safe shell string for this plan because one path contains characters that cannot be quoted safely; use the argv list instead")


def child_env(extra=None):
    """A fixed allowlist, so tokens and DYLD_* variables in the server's environment never reach a child."""
    env = {name: os.environ[name] for name in SAFE_ENV_NAMES if name in os.environ}
    env.setdefault("PATH", "/usr/local/bin:/usr/bin:/bin")
    env.update(extra or {})
    return env


def find_tool(name):
    for directory in os.environ.get("PATH", "/usr/local/bin:/usr/bin:/bin").split(":"):
        if directory.startswith("/"):
            candidate = os.path.join(directory, name)
            if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
                return candidate
    return None


def run_capped(argv, env, timeout, kill_on_timeout, cap=MAX_CHILD_OUTPUT):
    """Runs argv without a shell and without stdin. Output goes through a temp file so that abandoning
    a child never closes a pipe it is writing to. Returns (returncode or None, output, truncated, proc)."""
    with tempfile.TemporaryFile() as sink:
        proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=sink, stderr=subprocess.STDOUT, env=env, cwd="/", start_new_session=True, close_fds=True)
        deadline = time.monotonic() + timeout
        while proc.poll() is None and time.monotonic() < deadline:
            time.sleep(0.02)
        code = proc.poll()
        if code is None and kill_on_timeout:
            proc.kill()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                pass  # stuck in uninterruptible I/O; it is reaped later
            code = None
        sink.flush()
        size = os.fstat(sink.fileno()).st_size
        sink.seek(0)
        data = sink.read(cap)
    return code, data.decode("utf-8", "replace"), size > cap, proc


class Tool:
    def __init__(self, name, title, description, properties, required, handler, read_only, output_properties, destructive=False, shell_fields=()):
        self.name = name
        self.shell_fields = frozenset(shell_fields)
        self.handler = handler
        self.input_schema = {"type": "object", "properties": properties, "required": list(required), "additionalProperties": False}
        self.output_schema = {"type": "object", "properties": output_properties, "additionalProperties": True}
        self.annotations = {"title": title, "readOnlyHint": read_only, "destructiveHint": destructive, "idempotentHint": read_only, "openWorldHint": False}
        self.title = title
        self.description = description

    def listing(self):
        return {"name": self.name, "title": self.title, "description": self.description, "inputSchema": self.input_schema,
                "outputSchema": self.output_schema, "annotations": self.annotations}


def validate_value(schema, value, label):
    kind = schema.get("type")
    checks = {"string": lambda v: isinstance(v, str), "integer": lambda v: isinstance(v, int) and not isinstance(v, bool),
              "boolean": lambda v: isinstance(v, bool), "object": lambda v: isinstance(v, dict), "array": lambda v: isinstance(v, list)}
    if kind and not checks[kind](value):
        raise ToolError(f"argument {label!r} must be of type {kind}")
    if "enum" in schema and value not in schema["enum"]:
        raise ToolError(f"argument {label!r} must be one of {', '.join(map(str, schema['enum']))}")
    if kind == "string":
        if len(value) < schema.get("minLength", 0) or len(value) > schema.get("maxLength", 1 << 30):
            raise ToolError(f"argument {label!r} must be {schema.get('minLength', 0)} to {schema.get('maxLength', 'any number of')} characters")
        if not control_free(value):
            raise ToolError(f"argument {label!r} must not contain control characters")
    if kind == "integer" and not schema.get("minimum", value) <= value <= schema.get("maximum", value):
        raise ToolError(f"argument {label!r} must be between {schema.get('minimum')} and {schema.get('maximum')}")


def validate_arguments(tool, arguments):
    schema = tool.input_schema
    unknown = sorted(set(arguments) - set(schema["properties"]))
    if unknown:
        raise ToolError(f"unknown argument(s): {', '.join(unknown)}; accepted: {', '.join(schema['properties']) or 'none'}")
    for name in schema["required"]:
        if name not in arguments:
            raise ToolError(f"missing required argument {name!r}")
    for name, value in arguments.items():
        validate_value(schema["properties"][name], value, name)


def conforms(schema, value):
    """Minimal check that structured output matches the declared outputSchema (used by the tests)."""
    kinds = {"string": str, "integer": int, "boolean": bool, "object": dict, "array": list, "null": type(None)}
    expected = schema.get("type")
    if expected:
        allowed = expected if isinstance(expected, list) else [expected]
        if not any(isinstance(value, kinds[k]) and not (k == "integer" and isinstance(value, bool)) for k in allowed):
            return False
    if isinstance(value, dict):
        return all(conforms(sub, value[key]) for key, sub in schema.get("properties", {}).items() if key in value)
    return True


class Server:
    def __init__(self, config):
        self.config = config
        self.redact = Redactor(config.home)
        self.initialize_seen = False
        self.initialized = False
        self.children = []
        self.launch_stamp, self.launch_sequence = "", 0
        self.tools = {}
        for tool in build_tools(self):
            self.tools[tool.name] = tool

    # ---- protocol -------------------------------------------------------------------------------------------
    def handle_line(self, line):
        """Returns the response object for one input line, or None when none is due."""
        try:
            message = json.loads(line)
        except (ValueError, RecursionError):
            return self.error(None, -32700, "Parse error: the line is not valid JSON")
        if not isinstance(message, dict):
            return self.error(None, -32600, "Invalid request: JSON-RPC batches and non-object messages are not supported")
        if "method" not in message:
            return None  # a response to a request we never sent
        request_id = message.get("id")
        has_id = "id" in message
        if message.get("jsonrpc") != "2.0" or not isinstance(message["method"], str):
            return self.error(request_id if self.valid_id(request_id) else None, -32600, "Invalid request: jsonrpc must be '2.0' and method a string")
        if has_id and not self.valid_id(request_id):
            return self.error(None, -32600, "Invalid request: id must be a string or an integer")
        params = message.get("params", {})
        if not has_id:
            self.notification(message["method"], params)
            return None
        try:
            return {"jsonrpc": "2.0", "id": request_id, "result": self.dispatch(message["method"], params)}
        except RpcError as error:
            return self.error(request_id, error.code, error.message)
        except Exception as error:  # noqa: BLE001 - a bug in one request must not end the session
            log(f"internal error in {message['method']}: {self.redact.text(repr(error))}")
            return self.error(request_id, -32603, "Internal error")

    @staticmethod
    def valid_id(value):
        return isinstance(value, (str, int)) and not isinstance(value, bool)

    @staticmethod
    def error(request_id, code, message):
        return {"jsonrpc": "2.0", "id": request_id, "error": {"code": code, "message": message}}

    def notification(self, method, params):
        if method == "notifications/initialized" and self.initialize_seen:
            self.initialized = True

    def dispatch(self, method, params):
        if not isinstance(params, dict):
            raise RpcError(-32602, "params must be an object")
        if method == "ping":
            return {}
        if method == "initialize":
            return self.initialize(params)
        if not self.initialized:
            raise RpcError(-32002, "Server not initialized: send initialize, then notifications/initialized")
        if method == "tools/list":
            if params.get("cursor") is not None:
                raise RpcError(-32602, "Invalid cursor: this server returns every tool in one page")
            return {"tools": [tool.listing() for tool in self.tools.values()]}
        if method == "tools/call":
            return self.call_tool(params)
        raise RpcError(-32601, f"Method not found: {method}")

    def initialize(self, params):
        requested = params.get("protocolVersion")
        if not isinstance(requested, str):
            raise RpcError(-32602, "protocolVersion must be a string")
        if not isinstance(params.get("capabilities", {}), dict) or not isinstance(params.get("clientInfo", {}), dict):
            raise RpcError(-32602, "capabilities and clientInfo must be objects")
        self.initialize_seen = True
        return {
            "protocolVersion": requested if requested in SUPPORTED_PROTOCOLS else LATEST_PROTOCOL,
            "capabilities": {"tools": {"listChanged": False}},
            "serverInfo": {"name": SERVER_NAME, "title": "Darling Launcher", "version": SERVER_VERSION},
            "instructions": "Inspect the Darling Launcher's runtime, prefixes, apps, mounts and log with the read-only tools. "
                            "launch_app and stop_prefix only work when the user started this server with "
                            "DARLING_LAUNCHER_MCP_ALLOW_ACTIONS=1 and need confirm=true. Privileged mounts are never run here: "
                            "mount_plan returns the commands for the user to run.",
        }

    def call_tool(self, params):
        name, arguments = params.get("name"), params.get("arguments", {})
        if not isinstance(name, str):
            raise RpcError(-32602, "tools/call needs a string 'name'")
        if name not in self.tools:
            raise RpcError(-32602, f"Unknown tool: {name}")
        if arguments is None:
            arguments = {}
        if not isinstance(arguments, dict):
            raise RpcError(-32602, "'arguments' must be an object")
        tool = self.tools[name]
        self.reap_children()
        try:
            validate_arguments(tool, arguments)
            data, summary = tool.handler(arguments)
        except ToolError as error:
            return self.failure(str(error))
        except Exception as error:  # noqa: BLE001
            log(f"tool {name} crashed: {self.redact.text(repr(error))}")
            return self.failure(f"internal error in {name}: {type(error).__name__}")
        data = self.redact.obj(data, tool.shell_fields)
        text = self.redact.text(summary) + "\n\n" + json.dumps(data, indent=2, ensure_ascii=False)
        if len(text) > MAX_RESULT_CHARS:
            text = text[:MAX_RESULT_CHARS] + "\n[truncated]"
        return {"content": [{"type": "text", "text": text}], "structuredContent": data, "isError": False}

    def failure(self, message):
        return {"content": [{"type": "text", "text": self.redact.text(message)}], "isError": True}

    def reap_children(self):
        self.children = [c for c in self.children if c.poll() is None]

    def track(self, proc):
        self.reap_children()
        self.children.append(proc)
        del self.children[:-MAX_TRACKED_CHILDREN]

    # ---- shared helpers for tools ----------------------------------------------------------------------------
    def tail_file(self, path, count):
        try:
            fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        except FileNotFoundError:
            return []
        except OSError as error:
            raise ToolError(f"cannot open log: {error.strerror}")
        with os.fdopen(fd, "rb") as handle:
            info = os.fstat(handle.fileno())
            if not stat.S_ISREG(info.st_mode):
                raise ToolError("log is not a regular file")
            handle.seek(max(0, info.st_size - MAX_LOG_READ_BYTES))
            lines = handle.read(MAX_LOG_READ_BYTES).decode("utf-8", "replace").splitlines()
        kept, total = [], 0
        for line in reversed(lines[-count:]):
            total += min(len(line), MAX_LOG_LINE_CHARS)
            if total > MAX_LOG_TOTAL_CHARS:
                break
            kept.append(line[:MAX_LOG_LINE_CHARS])
        return kept[::-1]

    def launch_logs(self):
        """The per-launch log files, oldest first."""
        check_no_symlinks(self.config.data_dir, ["logs", LAUNCH_LOG_DIR])
        try:
            entries = os.scandir(self.config.log_dir / LAUNCH_LOG_DIR)
        except FileNotFoundError:
            return []
        with entries:
            return sorted(e.name for e in entries if LAUNCH_LOG_RE.match(e.name) and e.is_file(follow_symlinks=False))

    def log_lines(self, source, count):
        check_no_symlinks(self.config.data_dir, ["logs"])
        if source == "launcher":
            return self.tail_file(self.config.log_dir / "launcher.log", count)
        names = self.launch_logs()
        return self.tail_file(self.config.log_dir / LAUNCH_LOG_DIR / names[-1], count) if names else []

    def catalog(self, prefix):
        """Imported apps of a prefix from its private catalog, as (valid entries, number of skipped entries)."""
        check_no_symlinks(prefix, [".darling-launcher"])
        path = prefix / ".darling-launcher" / "catalog.json"
        if lstat_or_none(path) is None:
            return [], 0
        raw = read_regular_file(path, MAX_CATALOG_BYTES, "prefix catalog")
        try:
            document = json.loads(raw)
        except (ValueError, RecursionError, MemoryError):
            raise ToolError("the prefix catalog is not valid JSON, or is nested too deeply")
        records = document.get("apps", []) if isinstance(document, dict) else None
        if not isinstance(records, list):
            raise ToolError("the prefix catalog has no 'apps' list")
        apps, skipped = [], 0
        for record in records:
            if not isinstance(record, dict):
                skipped += 1
                continue
            name, bundle, executable = record.get("name"), record.get("bundle"), record.get("executable")
            parts = bundle.split("/") if isinstance(bundle, str) else []
            valid = (isinstance(name, str) and 0 < len(name) <= 256 and control_free(name)
                     and len(parts) >= 2 and parts[0] == "Applications" and parts[-1].endswith(".app")
                     and all(p not in ("", ".", "..") and control_free(p) for p in parts)
                     and isinstance(executable, str) and executable not in ("", ".", "..") and "/" not in executable and control_free(executable))
            if valid:
                apps.append({"name": name, "bundle": bundle, "executable": executable})
            else:
                skipped += 1
        return apps, skipped


class RpcError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code
        self.message = message


# ---- read-only tools ------------------------------------------------------------------------------------------
def build_tools(server):
    config = server.config

    def managed_prefixes():
        try:
            entries = sorted(os.scandir(config.prefix_root), key=lambda e: e.name)
        except FileNotFoundError:
            return [], 0
        except OSError as error:
            raise ToolError(f"cannot list the prefix root: {error.strerror}")
        found, skipped = [], 0
        for entry in entries:
            if entry.is_symlink() or not entry.is_dir(follow_symlinks=False) or not NAME_RE.match(entry.name):
                skipped += 1
            else:
                found.append(Path(entry.path))
        return found, skipped

    def runtimes():
        result = []
        try:
            entries = list(os.scandir(config.runtimes_root))
        except OSError:
            return result
        for entry in entries:
            match = TAG_RE.match(entry.name)
            if not match or entry.is_symlink() or not entry.is_dir(follow_symlinks=False):
                continue
            binary = Path(entry.path) / "usr/local/bin/darling"
            info = lstat_or_none(binary)
            usable = bool(info and stat.S_ISREG(info.st_mode) and os.access(binary, os.X_OK))
            result.append({"tag": entry.name, "usable": usable, "_key": (match[1], match[2], match[3], int(match[5] or 0))})
        result.sort(key=lambda r: r["_key"])
        for r in result:
            del r["_key"]
        return result

    def launcher_status(_args):
        settings = read_settings(config)
        installed = runtimes()
        usable = [r for r in installed if r["usable"]]
        prefixes, _ = managed_prefixes()
        selected = settings.get("prefix", "")
        default_prefix = selected or str(config.prefix_root / "default")
        managed = os.path.dirname(default_prefix) == str(config.prefix_root) and bool(NAME_RE.match(os.path.basename(default_prefix)))
        data = {
            "server": {"name": SERVER_NAME, "version": SERVER_VERSION, "protocol": LATEST_PROTOCOL},
            "actions_enabled": config.allow_actions,
            "data_dir": str(config.data_dir),
            "settings_file_found": config.config_file.is_file(),
            "selected": {k: settings.get(k, "") for k in ("prefix", "darling", "runtimeRoot", "volume")},
            "runtime": {"installed": bool(usable), "latest_tag": usable[-1]["tag"] if usable else "", "runtimes": installed},
            "default_prefix": {"path": default_prefix, "managed": managed, "exists": os.path.isdir(default_prefix)},
            "managed_prefix_count": len(prefixes),
            "mount_root": str(config.mount_root),
            "recent_log": server.log_lines("launcher", 10),
        }
        summary = (f"Runtime: {data['runtime']['latest_tag'] or 'none installed'}; {len(prefixes)} managed prefix(es); "
                   f"actions {'enabled' if config.allow_actions else 'disabled'}.")
        return data, summary

    def list_prefixes(_args):
        settings = read_settings(config)
        selected = os.path.basename(settings.get("prefix", "").rstrip("/"))
        found, skipped = managed_prefixes()
        items = []
        for path in found:
            try:
                apps, _ = server.catalog(path)
                count, problem = len(apps), ""
            except ToolError as error:
                count, problem = 0, str(error)
            items.append({"name": path.name, "path": str(path), "app_count": count, "selected": path.name == selected and
                          os.path.dirname(settings.get("prefix", "")) == str(config.prefix_root), "catalog_problem": problem})
        external = [p for p in settings.get("knownPrefixes", []) if os.path.dirname(p.rstrip("/")) != str(config.prefix_root)]
        data = {"prefix_root": str(config.prefix_root), "prefixes": items, "skipped_entries": skipped, "known_prefixes_outside_root": len(external)}
        return data, f"{len(items)} prefix(es) under the prefix root."

    def list_apps(args):
        prefix = resolve_prefix(config, args["prefix"])
        apps, skipped = server.catalog(prefix)
        return {"prefix": str(prefix), "apps": apps, "skipped_entries": skipped}, f"{len(apps)} imported app(s) in {prefix.name}."

    def mount_status(_args):
        findmnt = find_tool("findmnt")
        if not findmnt:
            raise ToolError("findmnt was not found on PATH")
        code, output, truncated, _ = run_capped([findmnt, "--json", "--list", "--output", "SOURCE,TARGET,FSTYPE,OPTIONS"], child_env(), FINDMNT_TIMEOUT, True, 4 * MAX_CHILD_OUTPUT)
        if code is None:
            raise ToolError(f"findmnt did not finish within {int(FINDMNT_TIMEOUT)} seconds")
        if code != 0 or truncated:
            raise ToolError(f"findmnt failed (exit {code}): {output[:500]}" if code != 0 else "findmnt output is too large")
        try:
            records = json.loads(output).get("filesystems", [])
        except (ValueError, AttributeError):
            raise ToolError("findmnt did not return valid JSON")
        roots = {str(config.mount_root)}
        try:
            roots.add(os.path.realpath(config.mount_root))
        except OSError:
            pass
        mounts = []
        for record in records if isinstance(records, list) else []:
            target = record.get("target") if isinstance(record, dict) else None
            if isinstance(target, str) and any(target == r or target.startswith(r + "/") for r in roots):
                options = [o for o in str(record.get("options", "")).split(",") if o]
                mounts.append({"source": str(record.get("source", "")), "target": target, "fstype": str(record.get("fstype", "")),
                               "options": options, "read_only": "ro" in options})
        mounts = mounts[:200]
        return {"mount_root": str(config.mount_root), "mounts": mounts}, f"{len(mounts)} mount(s) under the launcher mount root."

    def tail_log(args):
        lines = args.get("lines", 50)
        source = args.get("source", "launcher")
        found = server.log_lines(source, lines)
        return {"source": source, "lines": found}, f"Last {len(found)} line(s) of the {source} log."

    def require_actions(args):
        """The two gates every action passes before it touches anything: server-start opt-in, then per-call confirm."""
        if not config.allow_actions:
            raise ToolError("actions are disabled: the user must start this server with DARLING_LAUNCHER_MCP_ALLOW_ACTIONS=1")
        if args.get("confirm") is not True:
            raise ToolError("confirm must be true to run this action; ask the user before sending it")

    def darling_environment(settings, prefix):
        binary = settings.get("darling", "") or find_tool("darling") or ""
        if not binary:
            raise ToolError("no darling executable: select one in the launcher settings or put it on PATH")
        if not binary.startswith("/") or not control_free(binary) or not os.path.isfile(binary) or not os.access(binary, os.X_OK):
            raise ToolError(f"the configured darling is not an absolute path to an executable file: {binary}")
        extra = {"DPREFIX": str(prefix)}
        runtime_root = settings.get("runtimeRoot", "")
        if runtime_root:
            if not runtime_root.startswith("/") or not control_free(runtime_root) or not os.path.isdir(runtime_root):
                raise ToolError("the configured runtime install root is not an existing absolute directory")
            extra["DARLING_INSTALL_PREFIX"] = runtime_root
        return binary, child_env(extra)

    def find_app(prefix, reference):
        apps, _ = server.catalog(prefix)
        matches = [a for a in apps if reference in (a["name"], a["bundle"])]
        if not matches:
            raise ToolError(f"no imported app {reference!r} in prefix {prefix.name}; use list_apps")
        if len(matches) > 1:
            raise ToolError(f"{len(matches)} apps match {reference!r}; use one of the bundle paths: " + ", ".join(a["bundle"] for a in matches))
        app = matches[0]
        guest = f"{app['bundle']}/Contents/MacOS/{app['executable']}"
        check_no_symlinks(prefix, guest.split("/"))
        info = lstat_or_none(prefix / guest)
        if info is None or not stat.S_ISREG(info.st_mode):
            raise ToolError(f"the executable of {app['name']} is missing from the prefix")
        return app, "/" + guest

    def launch_app(args):
        require_actions(args)
        prefix = resolve_prefix(config, args["prefix"])
        binary, env = darling_environment(read_settings(config), prefix)
        app, guest = find_app(prefix, args["app"])
        server.launch_logs()  # refuses a symlinked logs directory
        log_dir = config.log_dir / LAUNCH_LOG_DIR
        log_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
        # UTC, so local-time changes (a DST fall-back repeats an hour) cannot reorder names; the stamp never goes behind this
        # process's previous launch, and a counter orders launches within one second, so the lexically last name is the newest.
        stamp = max(time.strftime("%Y%m%dT%H%M%S", time.gmtime()), server.launch_stamp)
        server.launch_sequence = server.launch_sequence + 1 if stamp == server.launch_stamp else 0
        server.launch_stamp = stamp
        log_name = f"{stamp}-{server.launch_sequence:04x}{secrets.token_hex(2)}.log"
        argv = [binary, "exec", guest]
        try:
            fd = os.open(log_dir / log_name, os.O_RDWR | os.O_APPEND | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        except OSError as error:
            raise ToolError(f"cannot create the launch log: {error.strerror}")
        try:
            os.write(fd, f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} [launch:{app['name']}] {prefix.name}: {' '.join(argv)}\n".encode())
            try:
                proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=fd, stderr=subprocess.STDOUT, env=env, cwd="/", start_new_session=True, close_fds=True)
            except OSError as error:
                raise ToolError(f"could not start darling: {error.strerror}")
            server.track(proc)
            deadline = time.monotonic() + args.get("wait_seconds", 5)
            while proc.poll() is None and time.monotonic() < deadline:
                time.sleep(0.02)
            code = proc.poll()
            output = os.pread(fd, MAX_CHILD_OUTPUT, 0).decode("utf-8", "replace")
        finally:
            os.close(fd)
        for old in server.launch_logs()[:-MAX_LAUNCH_LOGS]:
            try:
                if time.time() - os.lstat(log_dir / old).st_mtime >= MIN_PRUNE_AGE_SECONDS:
                    os.unlink(log_dir / old)  # only this server's own files, matched by name
            except OSError:
                pass
        data = {"prefix": str(prefix), "app": app["name"], "bundle": app["bundle"], "pid": proc.pid, "log_file": log_name,
                "state": "running" if code is None else "exited", "exit_code": code, "output": output}
        summary = (f"{app['name']} is still running (pid {proc.pid}); it was left running." if code is None
                   else f"{app['name']} exited with code {code}.")
        return data, summary

    def stop_prefix(args):
        require_actions(args)
        prefix = resolve_prefix(config, args["prefix"])
        binary, env = darling_environment(read_settings(config), prefix)
        code, output, truncated, proc = run_capped([binary, "shutdown"], env, SHUTDOWN_TIMEOUT, False)
        if code is None:
            server.track(proc)
            raise ToolError(f"darling shutdown has not finished after {int(SHUTDOWN_TIMEOUT)} seconds; it was left running (pid {proc.pid}), not killed")
        if code != 0:
            raise ToolError(f"darling shutdown failed (exit {code}): {output[:1000]}")
        return {"prefix": str(prefix), "exit_code": code, "output": output, "truncated": truncated}, f"Requested shutdown of all processes in {prefix.name}."

    def mount_plan(args):
        """Builds, and never runs, the commands for the user. The shapes mirror LauncherMount::makeCommand
        (src/mount.cpp), tools/mount-macos.py and, for the cryptex kind, makeCryptexCommand and
        tools/mount-cryptex.py as documented on branch feat/cryptex-mount-action; nothing is imported from there."""
        kind = args["kind"]
        extra = sorted(set(args) - {"kind"} - MOUNT_PLAN_ARGUMENTS[kind])
        if extra:
            raise ToolError(f"argument(s) {', '.join(extra)} are not used by kind {kind!r}; it takes {', '.join(sorted(MOUNT_PLAN_ARGUMENTS[kind]))}")
        warnings, commands = [], []

        def directory_warnings(path, must_be_empty):
            if not path.exists():
                warnings.append(f"{path} does not exist yet; create it (mkdir -p) before running the command" if must_be_empty else f"{path} does not exist")
            elif must_be_empty and any(path.iterdir()):
                warnings.append(f"{path} is not empty; the mount will be refused")

        if kind in ("batch", "cryptex"):
            root = resolve_under(config.mount_root, args.get("mount_root", str(config.mount_root)), "mount_root", True)
            if "device" in args and (not DEVICE_RE.match(args["device"]) or ".." in args["device"]):
                raise ToolError("device must be a detected device path under /dev")
            script_name = "mount-macos.py" if kind == "batch" else "mount-cryptex.py"
            # The script asks pkexec to run ITSELF as root, so it is offered only from a root-owned install location,
            # never from the checkout or any directory the user (or a guest) can write.
            script = trusted_file(INSTALLED_SCRIPT_DIRECTORIES, script_name, False)
            if script is None:
                warnings.append(f"No command is offered for kind {kind!r}: {script_name} is not installed in {' or '.join(INSTALLED_SCRIPT_DIRECTORIES)}. "
                                "The script runs as root through pkexec, so a copy in a checkout or home directory is never used. "
                                "This launcher build has no install rule for it yet: the script plans become available once the launcher's install rule "
                                "(the cryptex mount action, PR #24) is merged and `sudo cmake --install <build directory>` has been run. "
                                "The per-partition kinds 'partition' and 'unmount' need no script and work now.")
            else:
                argv = [system_tool("python3"), "-I", script, "--mount-root", str(root)]
                if kind == "cryptex":
                    argv += ["--apfs-fuse", system_tool("apfs-fuse")]
                else:
                    # Explicit empty values keep the script from falling back to a PATH lookup; HFS-only machines still work.
                    for flag, tool in (("--apfs-fuse", "apfs-fuse"), ("--apfsutil", "apfsutil")):
                        found = trusted_file(TRUSTED_DIRECTORIES, tool, True)
                        argv += [flag, found or ""]
                        if found is None:
                            warnings.append(f"{tool} was not found in {' or '.join(TRUSTED_DIRECTORIES)}; APFS partitions will report an error (HFS partitions are unaffected)")
                if kind == "batch":
                    description = "Mount every detected macOS partition read-only. The script asks for one pkexec authorization itself; run it in a desktop terminal."
                else:
                    if "device" in args:
                        argv += ["--device", args["device"]]
                    description = "Mount the Preboot volume (one pkexec request) and then the cryptex OS image read-only. Run it in a desktop terminal."
                directory_warnings(root, False)
                commands.append({"description": description, "run_as": "you (the script requests pkexec itself)", "argv": argv})
        elif kind == "partition":
            for name in ("device", "filesystem", "backend"):
                if name not in args:
                    raise ToolError(f"kind 'partition' needs {name}")
            device, filesystem, backend = args["device"], args["filesystem"], args["backend"]
            if not DEVICE_RE.match(device) or ".." in device:
                raise ToolError("device must be a detected device path under /dev")
            if filesystem == "apfs" and "volume_index" not in args:
                raise ToolError("select an APFS container volume index explicitly with volume_index")
            if backend == "apfs-fuse" and filesystem != "apfs":
                raise ToolError("the apfs-fuse backend only supports APFS partitions")
            directory = resolve_under(config.mount_root, args.get("directory", f"{config.mount_root}/{os.path.basename(device)}-manual"), "directory", False)
            directory_warnings(directory, True)
            options = f"ro,nodev,nosuid,noexec,uid={os.getuid()},gid={os.getgid()}"
            if backend == "apfs-fuse":
                argv = [system_tool("pkexec"), system_tool("apfs-fuse"), "-v", str(args["volume_index"]), "-o", options + ",allow_other", device, str(directory)]
            else:
                if filesystem == "apfs":
                    options += f",vol={args['volume_index']}"
                argv = [system_tool("pkexec"), system_tool("mount"), "-i", "-t", filesystem, "-o", options, "--", device, str(directory)]
            commands.append({"description": f"Mount {device} read-only at {directory} (nodev, nosuid, noexec).", "run_as": "root via pkexec", "argv": argv})
        else:
            if "directory" not in args:
                raise ToolError("kind 'unmount' needs directory")
            directory = resolve_under(config.mount_root, args["directory"], "directory", False)
            directory_warnings(directory, False)
            commands.append({"description": f"Unmount {directory}.", "run_as": "root via pkexec", "argv": [system_tool("pkexec"), system_tool("umount"), "--", str(directory)]})
        homes = home_expansions(config.home)
        notes = ['The "shell" strings use "$HOME" for your home directory: paste them into your own shell. The "argv" lists are for reading and show ~ instead.']
        for command in commands:
            # Secrets leave each element before it is quoted; nothing is ever redacted after quoting.
            safe = [server.redact.text(arg, paths=False) for arg in command["argv"]]
            if safe != command["argv"]:
                notes.append("Text in a path that looks like a secret was replaced by REDACTED in the shell string, so that command will not work until you correct it.")
            words = [shell_word(arg, homes) for arg in safe]
            for arg, word in zip(safe, words):
                check_shell_word(arg, word, homes)
            command["shell"] = " ".join(words)
            if any("'<user>'" in word for word in words):
                notes.append("A user name was replaced by the quoted placeholder '<user>' in a shell string: replace it with your user name before running it.")
        data = {"kind": kind, "executed": False, "commands": commands, "warnings": warnings, "notes": list(dict.fromkeys(notes))}
        return data, f"Plan for {kind}: {len(commands)} command(s). Nothing was run; the user must run them."

    read_only_note = "Read-only. "
    disabled_note = ("NOT read-only. Disabled unless the user started this server with DARLING_LAUNCHER_MCP_ALLOW_ACTIONS=1, and every call needs confirm=true "
                     "(ask the user first). ")
    prefix_property = {"type": "string", "minLength": 1, "maxLength": 1024, "description": "Prefix name or absolute path of a direct child of the prefix root (see list_prefixes)."}
    confirm_property = {"type": "boolean", "description": "Must be true. Set it only after the user agreed to this exact action."}
    return [
        Tool("launcher_status", "Launcher status",
             read_only_note + "Reports the installed prebuilt runtime tags and whether one is usable, the selected prefix/darling/runtime/volume from the launcher settings, "
             "the default prefix, the mount root, whether action tools are enabled, and the last log lines (home directory paths redacted).",
             {}, [], launcher_status, True,
             {"actions_enabled": {"type": "boolean"}, "runtime": {"type": "object"}, "recent_log": {"type": "array"}}),
        Tool("list_prefixes", "List prefixes",
             read_only_note + "Lists the managed prefixes (direct, non-symlink subdirectories of the launcher's prefix root) with their imported-app counts. "
             "Prefixes elsewhere are only counted, never addressable by the other tools.",
             {}, [], list_prefixes, True, {"prefixes": {"type": "array"}, "prefix_root": {"type": "string"}}),
        Tool("list_apps", "List imported apps",
             read_only_note + "Lists the apps recorded in a managed prefix's import catalog (name, bundle path, executable). Does not start anything.",
             {"prefix": {"type": "string", "minLength": 1, "maxLength": 1024, "description": "Prefix name, or absolute path of a direct child of the prefix root (see list_prefixes)."}},
             ["prefix"], list_apps, True, {"apps": {"type": "array"}, "prefix": {"type": "string"}}),
        Tool("mount_status", "Mount status",
             read_only_note + "Runs findmnt (no privileges) and returns the mounts under the launcher's mount root with source, filesystem, options and a read_only flag. Never mounts or unmounts.",
             {}, [], mount_status, True, {"mounts": {"type": "array"}, "mount_root": {"type": "string"}}),
        Tool("tail_log", "Tail launcher log",
             read_only_note + "Returns the last lines of the launcher log (source 'launcher') or of the newest app started by this server (source 'mcp-launch'), "
             "with home directory paths and token-like strings redacted.",
             {"lines": {"type": "integer", "minimum": 1, "maximum": MAX_LOG_LINES, "description": "Number of lines, default 50."},
              "source": {"type": "string", "enum": list(LOG_SOURCES), "description": "Which log, default 'launcher'; 'mcp-launch' is the newest launch_app log."}},
             [], tail_log, True, {"lines": {"type": "array"}, "source": {"type": "string"}}),
        Tool("mount_plan", "Plan a macOS volume mount",
             read_only_note + "Returns, and never runs, the exact command line the launcher would use so the USER can run it: kind 'batch' (mount all detected partitions), "
             "'partition' (one partition with pkexec mount or apfs-fuse), 'cryptex' (Preboot plus os.dmg) or 'unmount'. All mounts are read-only with nodev, nosuid and "
             "noexec. This server never calls sudo or pkexec. Helper programs (pkexec, mount, umount, python3, apfs-fuse, apfsutil) are only taken from root-owned "
             "files in /usr/local/bin or /usr/bin and cannot be chosen by the caller. The mount scripts of kinds 'batch' and 'cryptex' run as root through pkexec, so those kinds "
             "return a command only when the script is installed root-owned in <libexecdir>/darling-launcher, which needs the launcher's install rule from PR #24 (until it is merged "
             "and installed they return a warning and no command; kinds 'partition' and 'unmount' work now). Directories must be inside the launcher mount root; any other argument is rejected. "
             "Fields are redacted like every result (home paths shown as ~); the 'shell' string of each command is the copy-paste form for the user's own terminal "
             "and refers to the home directory as \"$HOME\", so run it in the user's shell. A user-name component that had to be hidden appears as the quoted "
             "placeholder '<user>' and is listed in notes.",
             {"kind": {"type": "string", "enum": list(MOUNT_PLAN_ARGUMENTS)},
              "mount_root": {"type": "string", "maxLength": 1024, "description": "kinds batch, cryptex: mount root, default the launcher mount root."},
              "device": {"type": "string", "maxLength": 256, "description": "kinds partition, cryptex: block device such as /dev/nvme0n1p5."},
              "filesystem": {"type": "string", "enum": ["apfs", "hfs", "hfsplus"], "description": "kind partition."},
              "backend": {"type": "string", "enum": ["kernel", "apfs-fuse"], "description": "kind partition."},
              "volume_index": {"type": "integer", "minimum": 0, "maximum": 99, "description": "kind partition: APFS container volume index (required for apfs)."},
              "directory": {"type": "string", "maxLength": 1024, "description": "kinds partition (mount directory) and unmount (directory to unmount), inside the mount root."}},
             ["kind"], mount_plan, True, {"executed": {"type": "boolean"}, "commands": {"type": "array"}, "warnings": {"type": "array"}}, shell_fields=("shell",)),
        Tool("launch_app", "Launch imported app",
             disabled_note + "Starts an imported app like the launcher's Launch button, with the `darling` selected in the launcher settings "
             "(or, if none is selected, the first `darling` on PATH): `darling exec /Applications/<App>.app/Contents/MacOS/<executable>` with DPREFIX set. "
             "Waits up to wait_seconds, then reports whether it exited or is still running; a running app is left running and is never killed. "
             "Its output goes to its own file under logs/mcp-launch (newest: tail_log source 'mcp-launch'); the file grows without a size cap while the app runs, and "
             "only files beyond the newest 20 that were last written over an hour ago are deleted.",
             {"prefix": prefix_property, "app": {"type": "string", "minLength": 1, "maxLength": 256, "description": "App name or bundle path from list_apps."},
              "confirm": confirm_property, "wait_seconds": {"type": "integer", "minimum": 0, "maximum": 60, "description": "How long to wait for an early exit, default 5."}},
             ["prefix", "app", "confirm"], launch_app, False, {"state": {"type": "string"}, "pid": {"type": "integer"}, "output": {"type": "string"}}),
        Tool("stop_prefix", "Stop prefix processes",
             disabled_note + "Runs `darling shutdown` for the prefix (with the `darling` from the launcher settings, or the first on PATH), which ends every app running in that prefix. It asks Darling to shut down; it never "
             "sends signals itself, and if the command does not finish in 60 seconds it is left running.",
             {"prefix": prefix_property, "confirm": confirm_property},
             ["prefix", "confirm"], stop_prefix, False, {"exit_code": {"type": "integer"}, "output": {"type": "string"}}, destructive=True),
    ]


# ---- stdio loop -----------------------------------------------------------------------------------------------
def read_line(stream):
    """Returns the next line, b'' at EOF, or None for a line over MAX_LINE_BYTES (the rest is discarded)."""
    line = stream.readline(MAX_LINE_BYTES + 1)
    if len(line) > MAX_LINE_BYTES and not line.endswith(b"\n"):
        while True:
            chunk = stream.readline(MAX_LINE_BYTES)
            if not chunk or chunk.endswith(b"\n"):
                break
        return None
    return line


def serve(server, stdin, stdout):
    while True:
        line = read_line(stdin)
        if line == b"":
            return 0
        if line is None:
            response = Server.error(None, -32600, f"Invalid request: line exceeds {MAX_LINE_BYTES} bytes")
        elif not line.strip():
            continue
        else:
            try:
                response = server.handle_line(line)
            except RecursionError:
                response = Server.error(None, -32600, "Invalid request: message is nested too deeply")
        if response is not None:
            try:
                stdout.write((json.dumps(response, separators=(",", ":")) + "\n").encode("utf-8"))
                stdout.flush()
            except BrokenPipeError:
                return 0


def main():
    try:
        config = Config.from_environ(os.environ)
    except ConfigError as error:
        log(f"configuration error: {error}")
        return 2
    log(f"started; data dir {config.data_dir}; actions {'enabled' if config.allow_actions else 'disabled'}")
    return serve(Server(config), sys.stdin.buffer, sys.stdout.buffer)


if __name__ == "__main__":
    sys.exit(main())
