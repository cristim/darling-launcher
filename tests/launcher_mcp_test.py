#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for tools/launcher-mcp.py. Fixture based: temp dirs and fake executables only, no Darling, pkexec or network."""
import ast
import calendar
import importlib.util
import json
import os
from pathlib import Path
import re
import select
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "tools/launcher-mcp.py"
spec = importlib.util.spec_from_file_location("launcher_mcp", SCRIPT)
mcp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mcp)

USER = "alice"
READ_TIMEOUT = 15


class Fixture:
    """A fake home with a launcher data dir, settings file and fake executables on PATH."""

    def __init__(self, allow_actions=False, home_name=USER, data_dir=None):
        self.root = Path(os.path.realpath(tempfile.mkdtemp(prefix="launcher-mcp-test-")))
        self.home = self.root / "home" / home_name
        self.data = self.root / data_dir if data_dir else self.home / ".darling-launcher"
        self.config_file = self.home / ".config/cristim/darling-launcher.conf"
        self.bin = self.root / "bin"
        for directory in (self.data / "prefixes", self.data / "logs", self.data / "mounts", self.data / "runtimes", self.config_file.parent, self.bin):
            directory.mkdir(parents=True)
        self.config = mcp.Config(self.home, self.data, self.config_file, allow_actions)
        self.server = mcp.Server(self.config)
        self.env = patch.dict(os.environ, {"PATH": f"{self.bin}:/usr/bin:/bin", "HOME": str(self.home)})
        self.env.start()
        self.server.initialized = True

    def close(self):
        for child in self.server.children:
            child.wait(timeout=10)
        self.env.stop()
        shutil.rmtree(self.root, ignore_errors=True)

    def settings(self, **values):
        self.config_file.write_text("[General]\n" + "".join(f"{k}={v}\n" for k, v in values.items()))

    def prefix(self, name="default", apps=()):
        path = self.data / "prefixes" / name
        (path / ".darling-launcher").mkdir(parents=True)
        records = []
        for app, executable in apps:
            bundle = f"Applications/{app}.app"
            (path / bundle / "Contents/MacOS").mkdir(parents=True)
            script = path / bundle / "Contents/MacOS" / executable
            script.write_text("#!/bin/sh\n")
            script.chmod(0o755)
            records.append({"name": app, "bundle": bundle, "executable": executable, "source": bundle, "chain": []})
        (path / ".darling-launcher/catalog.json").write_text(json.dumps({"apps": records}))
        return path

    def executable(self, name, body):
        path = self.bin / name
        path.write_text("#!/bin/sh\n" + body)
        path.chmod(0o755)
        return path

    def fake_findmnt(self, filesystems):
        self.executable("findmnt", "cat <<'JSON'\n" + json.dumps({"filesystems": filesystems}) + "\nJSON\n")

    def call(self, name, arguments=None):
        response = self.server.handle_line(json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": name, "arguments": arguments or {}}}))
        return response["result"] if "result" in response else response


class ToolTestCase(unittest.TestCase):
    fixture_args = {}

    def setUp(self):
        self.fx = Fixture(**self.fixture_args)
        self.addCleanup(self.fx.close)

    def ok(self, name, arguments=None):
        result = self.fx.call(name, arguments)
        self.assertFalse(result.get("isError"), result)
        tool = self.fx.server.tools[name]
        self.assertTrue(mcp.conforms(tool.output_schema, result["structuredContent"]))
        return result["structuredContent"]

    def refused(self, name, arguments, fragment):
        result = self.fx.call(name, arguments)
        self.assertTrue(result.get("isError"), result)
        self.assertIn(fragment, result["content"][0]["text"])
        return result


class Session:
    """The real server as a subprocess, for protocol conformance."""

    def __init__(self, env=None):
        self.root = Path(os.path.realpath(tempfile.mkdtemp(prefix="launcher-mcp-proto-")))
        environment = {"PATH": os.environ["PATH"], "HOME": str(self.root)}
        environment.update(env or {})
        self.buffer = b""
        self.proc = subprocess.Popen([sys.executable, "-I", str(SCRIPT)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment)

    def send_raw(self, data):
        self.proc.stdin.write(data)
        self.proc.stdin.flush()

    def send(self, message):
        self.send_raw(json.dumps(message).encode() + b"\n")

    def read_line(self, timeout=READ_TIMEOUT):
        """One line from the server, or b'' at EOF. A server that stops answering fails the test instead of hanging the suite."""
        deadline = time.monotonic() + timeout
        fd = self.proc.stdout.fileno()
        while b"\n" not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([fd], [], [], remaining)[0]:
                self.proc.kill()
                raise AssertionError(f"no response from the server within {timeout} seconds")
            chunk = os.read(fd, 65536)
            if not chunk:
                line, self.buffer = self.buffer, b""
                return line
            self.buffer += chunk
        line, _, self.buffer = self.buffer.partition(b"\n")
        return line + b"\n"

    def read(self):
        line = self.read_line()
        return json.loads(line) if line else None

    def read_to_eof(self, timeout=READ_TIMEOUT):
        data = b""
        while True:
            line = self.read_line(timeout)
            if not line:
                return data
            data += line

    def rpc(self, method, params=None, request_id=1):
        message = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            message["params"] = params
        self.send(message)
        return self.read()

    def handshake(self, version="2025-06-18"):
        response = self.rpc("initialize", {"protocolVersion": version, "capabilities": {}, "clientInfo": {"name": "test", "version": "0"}})
        self.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        return response

    def close(self):
        self.proc.stdin.close()
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        stderr = self.proc.stderr.read().decode()
        self.proc.stdout.close()
        self.proc.stderr.close()
        shutil.rmtree(self.root, ignore_errors=True)
        return stderr


class ProtocolTest(unittest.TestCase):
    def session(self, env=None):
        session = Session(env)
        self.addCleanup(session.close)
        return session

    def test_initialize_handshake_and_negotiation(self):
        session = self.session()
        result = session.handshake()["result"]
        self.assertEqual(result["protocolVersion"], "2025-06-18")
        self.assertEqual(result["serverInfo"]["name"], "darling-launcher-mcp")
        self.assertIn("tools", result["capabilities"])
        self.assertEqual(session.rpc("ping", request_id=2), {"jsonrpc": "2.0", "id": 2, "result": {}})
        older = self.session()
        self.assertEqual(older.handshake("2025-03-26")["result"]["protocolVersion"], "2025-03-26")
        unknown = self.session()
        self.assertEqual(unknown.handshake("1999-01-01")["result"]["protocolVersion"], "2025-06-18")

    def test_initialize_requires_a_version_string(self):
        session = self.session()
        self.assertEqual(session.rpc("initialize", {"capabilities": {}})["error"]["code"], -32602)

    def test_methods_before_initialization_are_refused_but_ping_works(self):
        session = self.session()
        self.assertEqual(session.rpc("ping")["result"], {})
        self.assertEqual(session.rpc("tools/list", request_id=2)["error"]["code"], -32002)

    def test_unknown_method_and_notifications(self):
        session = self.session()
        session.handshake()
        self.assertEqual(session.rpc("nope/nothing", request_id=7)["error"], {"code": -32601, "message": "Method not found: nope/nothing"})
        session.send({"jsonrpc": "2.0", "method": "notifications/unknown"})
        session.send({"jsonrpc": "2.0", "method": "nope"})
        self.assertEqual(session.rpc("ping", request_id=8)["id"], 8)

    def test_malformed_input_does_not_end_the_session(self):
        session = self.session()
        session.handshake()
        session.send_raw(b"{not json\n")
        self.assertEqual(session.read()["error"]["code"], -32700)
        session.send_raw(b"[1,2]\n")
        self.assertEqual(session.read()["error"]["code"], -32600)
        session.send_raw(b'{"jsonrpc":"1.0","id":3,"method":"ping"}\n')
        self.assertEqual(session.read()["error"]["code"], -32600)
        session.send_raw(b'{"jsonrpc":"2.0","id":true,"method":"ping"}\n')
        self.assertEqual(session.read()["error"]["code"], -32600)
        session.send_raw(b'{"jsonrpc":"2.0","id":4,"method":"ping","params":[]}\n')
        self.assertEqual(session.read()["error"]["code"], -32602)
        session.send_raw(b"\n\n")
        self.assertEqual(session.rpc("ping", request_id=9)["id"], 9)

    def test_oversized_line_is_rejected_and_discarded(self):
        session = self.session()
        session.handshake()
        session.send_raw(b'{"jsonrpc":"2.0","id":1,"method":"ping","params":{"x":"' + b"a" * (mcp.MAX_LINE_BYTES + 10) + b'"}}\n')
        error = session.read()["error"]
        self.assertEqual(error["code"], -32600)
        self.assertIn("exceeds", error["message"])
        self.assertEqual(session.rpc("ping", request_id=2)["id"], 2)

    def test_deeply_nested_json_is_rejected(self):
        session = self.session()
        session.handshake()
        session.send_raw(b"[" * 100000 + b"\n")
        self.assertIn(session.read()["error"]["code"], (-32600, -32700))
        self.assertEqual(session.rpc("ping", request_id=2)["id"], 2)

    def test_tools_list_schemas_are_valid(self):
        session = self.session()
        session.handshake()
        tools = session.rpc("tools/list")["result"]["tools"]
        self.assertEqual({t["name"] for t in tools}, set(mcp.Server(mcp.Config("/h", "/h/d", "/h/c", False)).tools))
        for tool in tools:
            self.assertRegex(tool["name"], r"^[a-z][a-z0-9_]{0,63}$")
            self.assertTrue(tool["description"])
            schema = tool["inputSchema"]
            self.assertEqual(schema["type"], "object")
            self.assertIs(schema["additionalProperties"], False)
            self.assertLessEqual(set(schema["required"]), set(schema["properties"]))
            for name, prop in schema["properties"].items():
                self.assertIn(prop["type"], ("string", "integer", "boolean"), name)
            self.assertEqual(tool["outputSchema"]["type"], "object")
            annotations = tool["annotations"]
            for hint in ("readOnlyHint", "destructiveHint", "idempotentHint", "openWorldHint"):
                self.assertIsInstance(annotations[hint], bool)
            if annotations["readOnlyHint"]:
                self.assertTrue(tool["description"].startswith("Read-only."))
            json.dumps(tool)

    def test_initialized_notification_alone_does_not_skip_the_handshake(self):
        session = self.session()
        session.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        self.assertEqual(session.rpc("tools/list", request_id=2)["error"]["code"], -32002)

    def test_tools_list_rejects_a_cursor(self):
        session = self.session()
        session.handshake()
        self.assertEqual(session.rpc("tools/list", {"cursor": "x"})["error"]["code"], -32602)

    def test_tools_call_protocol_errors(self):
        session = self.session()
        session.handshake()
        self.assertEqual(session.rpc("tools/call", {"name": "no_such_tool"})["error"]["code"], -32602)
        self.assertEqual(session.rpc("tools/call", {})["error"]["code"], -32602)
        self.assertEqual(session.rpc("tools/call", {"name": "launcher_status", "arguments": []})["error"]["code"], -32602)

    def test_tools_call_validation_failures_are_error_results(self):
        session = self.session()
        session.handshake()
        for name, arguments, fragment in (("launcher_status", {"extra": 1}, "unknown argument"), ("list_apps", {}, "missing required"),
                                          ("tail_log", {"lines": "5"}, "must be of type integer"), ("tail_log", {"lines": 0}, "between 1 and"),
                                          ("tail_log", {"lines": True}, "must be of type integer"), ("tail_log", {"source": "../x"}, "must be one of")):
            result = session.rpc("tools/call", {"name": name, "arguments": arguments})["result"]
            self.assertTrue(result["isError"], name)
            self.assertIn(fragment, result["content"][0]["text"])

    def test_bad_environment_fails_at_startup(self):
        for env in ({"DARLING_LAUNCHER_DATA_DIR": "relative/path"}, {"DARLING_LAUNCHER_DATA_DIR": "/a/../b"}, {"DARLING_LAUNCHER_MCP_ALLOW_ACTIONS": "yes"}):
            session = Session(env)
            self.assertEqual(session.proc.wait(timeout=READ_TIMEOUT), 2)
            self.assertIn("configuration error", session.close())

    def test_stdout_carries_only_protocol_and_logs_go_to_stderr(self):
        session = self.session()
        session.handshake()
        session.rpc("tools/call", {"name": "launcher_status", "arguments": {}}, request_id=2)
        session.proc.stdin.close()
        rest = session.read_to_eof()
        self.assertEqual(rest, b"")
        self.assertIn(b"started", session.proc.stderr.read())


class ReadOnlyToolsTest(ToolTestCase):
    def test_launcher_status(self):
        fx = self.fx
        (fx.data / "runtimes/v2026.10.01-abcdef0/usr/local/bin").mkdir(parents=True)
        binary = fx.data / "runtimes/v2026.10.01-abcdef0/usr/local/bin/darling"
        binary.write_text("#!/bin/sh\n")
        binary.chmod(0o755)
        (fx.data / "runtimes/v2026.10.02-abcdef1-r2/usr/local/bin").mkdir(parents=True)
        (fx.data / "runtimes/not-a-tag").mkdir()
        fx.prefix("default")
        fx.settings(prefix=str(fx.data / "prefixes/default"), darling=str(binary))
        (fx.data / "logs/launcher.log").write_text("one\ntwo\n")
        data = self.ok("launcher_status")
        self.assertTrue(data["runtime"]["installed"])
        self.assertEqual(data["runtime"]["latest_tag"], "v2026.10.01-abcdef0")
        self.assertEqual([r["tag"] for r in data["runtime"]["runtimes"]], ["v2026.10.01-abcdef0", "v2026.10.02-abcdef1-r2"])
        self.assertEqual([r["usable"] for r in data["runtime"]["runtimes"]], [True, False])
        self.assertTrue(data["default_prefix"]["managed"])
        self.assertEqual(data["managed_prefix_count"], 1)
        self.assertEqual(data["recent_log"], ["one", "two"])
        self.assertFalse(data["actions_enabled"])

    def test_launcher_status_on_an_empty_install(self):
        data = self.ok("launcher_status")
        self.assertFalse(data["runtime"]["installed"])
        self.assertEqual(data["recent_log"], [])
        self.assertFalse(data["settings_file_found"])

    def test_launcher_status_reports_an_unmanaged_default_prefix(self):
        self.fx.settings(prefix=str(self.fx.root / "elsewhere"))
        self.assertFalse(self.ok("launcher_status")["default_prefix"]["managed"])

    def test_settings_parsing_handles_quotes_lists_and_other_sections(self):
        self.fx.config_file.write_text('[General]\nprefix="/a b/c"\nknownPrefixes="/x, /y", /z\nwindowGeometry=@ByteArray(abc)\n[Other]\nprefix=/ignored\n')
        settings = mcp.read_settings(self.fx.config)
        self.assertEqual(settings["prefix"], "/a b/c")
        self.assertEqual(settings["knownPrefixes"], ["/x, /y", "/z"])

    def test_list_prefixes(self):
        fx = self.fx
        fx.prefix("default", [("Calc", "Calculator")])
        fx.prefix("second")
        (fx.data / "prefixes/not-a-dir").write_text("x")
        (fx.data / "prefixes/link").symlink_to(fx.root)
        (fx.data / "prefixes/.hidden").mkdir()
        (fx.data / "prefixes/bad..name").mkdir()
        (fx.data / "prefixes/second/.darling-launcher/catalog.json").write_text("{broken")
        fx.settings(prefix=str(fx.data / "prefixes/default"), knownPrefixes=f"{fx.root}/ext")
        data = self.ok("list_prefixes")
        by_name = {p["name"]: p for p in data["prefixes"]}
        self.assertEqual(set(by_name), {"default", "second"})
        self.assertEqual(by_name["default"]["app_count"], 1)
        self.assertTrue(by_name["default"]["selected"])
        self.assertIn("valid JSON", by_name["second"]["catalog_problem"])
        self.assertEqual(data["skipped_entries"], 4)
        self.assertEqual(data["known_prefixes_outside_root"], 1)

    def test_list_prefixes_without_a_prefix_root(self):
        shutil.rmtree(self.fx.data / "prefixes")
        self.assertEqual(self.ok("list_prefixes")["prefixes"], [])

    def test_list_apps_by_name_and_by_path(self):
        prefix = self.fx.prefix("default", [("Calc", "Calculator"), ("Notes", "Notes")])
        for ref in ("default", str(prefix)):
            apps = self.ok("list_apps", {"prefix": ref})["apps"]
            self.assertEqual(apps, [{"name": "Calc", "bundle": "Applications/Calc.app", "executable": "Calculator"},
                                    {"name": "Notes", "bundle": "Applications/Notes.app", "executable": "Notes"}])

    def test_list_apps_skips_unsafe_catalog_entries(self):
        prefix = self.fx.prefix("default", [("Good", "Good")])
        records = json.loads((prefix / ".darling-launcher/catalog.json").read_text())["apps"]
        bad = [{"name": "A", "bundle": "../x.app", "executable": "a"}, {"name": "B", "bundle": "Applications/B.app", "executable": "../b"},
               {"name": "C", "bundle": "/Applications/C.app", "executable": "c"}, {"name": "D", "bundle": "Applications/D.app", "executable": "a/b"},
               {"name": "E", "bundle": "Other/E.app", "executable": "e"}, {"name": "G", "bundle": "Applications/../x.app", "executable": "g"},
               {"name": "H", "bundle": "Applications/./x.app", "executable": "h"}, {"name": "I", "bundle": "Applications//x.app", "executable": "i"}, "text", {"name": 3, "bundle": "Applications/F.app", "executable": "f"}]
        (prefix / ".darling-launcher/catalog.json").write_text(json.dumps({"apps": records + bad}))
        data = self.ok("list_apps", {"prefix": "default"})
        self.assertEqual([a["name"] for a in data["apps"]], ["Good"])
        self.assertEqual(data["skipped_entries"], len(bad))

    def test_list_apps_without_catalog_and_with_broken_catalog(self):
        prefix = self.fx.data / "prefixes/empty"
        prefix.mkdir()
        self.assertEqual(self.ok("list_apps", {"prefix": "empty"})["apps"], [])
        (prefix / ".darling-launcher").mkdir()
        (prefix / ".darling-launcher/catalog.json").write_text('{"apps": 3}')
        self.refused("list_apps", {"prefix": "empty"}, "no 'apps' list")

    def test_one_deeply_nested_catalog_does_not_break_the_other_prefixes(self):
        self.fx.prefix("good", [("Calc", "Calculator")])
        bad = self.fx.prefix("bad")
        (bad / ".darling-launcher/catalog.json").write_text("[" * 200000)
        data = self.ok("list_prefixes")
        by_name = {p["name"]: p for p in data["prefixes"]}
        self.assertEqual(by_name["good"]["app_count"], 1)
        self.assertEqual(by_name["good"]["catalog_problem"], "")
        self.assertIn("nested too deeply", by_name["bad"]["catalog_problem"])
        self.refused("list_apps", {"prefix": "bad"}, "nested too deeply")
        (bad / ".darling-launcher/catalog.json").write_text('{"apps": ' + "[" * 100000 + "]" * 100000 + "}")
        self.refused("list_apps", {"prefix": "bad"}, "nested too deeply")
        self.assertEqual(len(self.ok("list_apps", {"prefix": "good"})["apps"]), 1)

    def test_catalog_symlinks_and_size_are_refused(self):
        prefix = self.fx.prefix("default")
        catalog = prefix / ".darling-launcher/catalog.json"
        outside = self.fx.root / "outside.json"
        outside.write_text('{"apps": []}')
        catalog.unlink()
        catalog.symlink_to(outside)
        self.refused("list_apps", {"prefix": "default"}, "cannot open prefix catalog")
        catalog.unlink()
        catalog.write_text("x" * (mcp.MAX_CATALOG_BYTES + 1))
        self.refused("list_apps", {"prefix": "default"}, "larger than")
        shutil.rmtree(prefix / ".darling-launcher")
        (prefix / ".darling-launcher").symlink_to(self.fx.root)
        self.refused("list_apps", {"prefix": "default"}, "refusing symlink")

    def test_prefix_path_escapes_are_refused(self):
        fx = self.fx
        fx.prefix("default")
        outside = fx.root / "outside-prefix"
        (outside / ".darling-launcher").mkdir(parents=True)
        (fx.data / "prefixes/escape").symlink_to(outside)
        (fx.data / "prefixes/dirlink").mkdir()
        cases = {
            "..": "invalid prefix name",
            "../default": "bare name or an absolute path",
            "default/..": "bare name or an absolute path",
            "a/b": "bare name or an absolute path",
            "": "1 to 1024",
            "x" * 1025: "1 to 1024",
            "bad\nname": "control characters",
            ".hidden": "invalid prefix name",
            "missing": "does not exist",
            "escape": "refusing prefix that is a symlink",
            str(outside): "outside the launcher prefix root",
            "/etc": "outside the launcher prefix root",
            "/": "outside the launcher prefix root",
            str(fx.data / "prefixes/../prefixes/default"): "'.', '..'",
            str(fx.data / "prefixes/default/sub"): "outside the launcher prefix root",
            str(fx.data / "prefixes/escape"): "refusing prefix that is a symlink",
            f"{fx.data}/prefixes//default": "empty components",
        }
        for value, fragment in cases.items():
            self.refused("list_apps", {"prefix": value}, fragment)
        self.refused("list_apps", {"prefix": 5}, "must be of type string")

    def test_prefix_root_behind_a_symlink_is_resolved_not_trusted_for_children(self):
        fx = self.fx
        real = fx.root / "real-prefixes"
        shutil.rmtree(fx.data / "prefixes")
        (real / "default/.darling-launcher").mkdir(parents=True)
        (fx.data / "prefixes").symlink_to(real)
        self.assertEqual(self.ok("list_apps", {"prefix": "default"})["apps"], [])
        self.assertEqual(self.ok("list_apps", {"prefix": str(real / "default")})["apps"], [])

    def test_mount_status_filters_to_the_mount_root(self):
        fx = self.fx
        inside = str(fx.data / "mounts/disk1-volume-0")
        fx.fake_findmnt([
            {"source": "/dev/nvme0n1p5", "target": inside, "fstype": "fuse.apfs-fuse", "options": "ro,nodev,nosuid,noexec"},
            {"source": "/dev/sda1", "target": str(fx.data / "mounts/rw"), "fstype": "hfsplus", "options": "rw,nodev"},
            {"source": "/dev/sda2", "target": str(fx.data / "mountsX/no"), "fstype": "ext4", "options": "rw"},
            {"source": "/dev/sda3", "target": "/", "fstype": "ext4", "options": "rw"},
            {"source": "x", "target": 5}, "garbage"])
        data = self.ok("mount_status")
        self.assertEqual([m["target"] for m in data["mounts"]], ["~/.darling-launcher/mounts/disk1-volume-0", "~/.darling-launcher/mounts/rw"])
        self.assertEqual([m["read_only"] for m in data["mounts"]], [True, False])
        self.assertEqual(data["mounts"][0]["options"], ["ro", "nodev", "nosuid", "noexec"])

    def test_mount_status_errors(self):
        fx = self.fx
        with patch.object(mcp, "find_tool", return_value=None):
            self.refused("mount_status", {}, "findmnt was not found")
        fx.executable("findmnt", "echo broken >&2\nexit 3\n")
        self.refused("mount_status", {}, "failed (exit 3)")
        fx.executable("findmnt", "echo 'not json'\n")
        self.refused("mount_status", {}, "valid JSON")
        fx.executable("findmnt", "yes x | head -c 400000\n")
        self.refused("mount_status", {}, "too large")

    def test_mount_status_times_out_and_kills_its_own_helper(self):
        pid_file = self.fx.root / "findmnt.pid"
        self.fx.executable("findmnt", f'echo $$ > "{pid_file}"\nexec sleep 30\n')
        started = time.monotonic()
        with patch.object(mcp, "FINDMNT_TIMEOUT", 0.5):
            self.refused("mount_status", {}, "did not finish")
        self.assertLess(time.monotonic() - started, 5)
        pid = int(pid_file.read_text())
        with self.assertRaises(ProcessLookupError):
            os.kill(pid, 0)

    def test_the_result_text_is_capped(self):
        (self.fx.data / "logs/launcher.log").write_text("".join(f"line {i} " + "y" * 90 + "\n" for i in range(200)))
        with patch.object(mcp, "MAX_RESULT_CHARS", 1000):
            text = self.fx.call("tail_log", {"lines": 200})["content"][0]["text"]
        self.assertEqual(len(text), 1000 + len("\n[truncated]"))
        self.assertTrue(text.endswith("\n[truncated]"))
        self.assertLess(len(text), len(self.fx.call("tail_log", {"lines": 200})["content"][0]["text"]))

    def test_tail_log_limits_and_sources(self):
        fx = self.fx
        (fx.data / "logs/launcher.log").write_text("".join(f"line {i}\n" for i in range(300)) + "x" * 5000 + "\n")
        (fx.data / "logs/mcp-launch").mkdir()
        (fx.data / "logs/mcp-launch/20260101T000000-aaaaaaaa.log").write_text("old launch\n")
        (fx.data / "logs/mcp-launch/20260101T000001-bbbbbbbb.log").write_text("launch output\n")
        (fx.data / "logs/mcp-launch/notes.txt").write_text("ignored\n")
        default = self.ok("tail_log")["lines"]
        self.assertEqual(len(default), 50)
        lines = self.ok("tail_log", {"lines": 3})["lines"]
        self.assertEqual(lines[:2], ["line 298", "line 299"])
        self.assertEqual(len(lines[2]), mcp.MAX_LOG_LINE_CHARS)
        self.assertEqual(len(self.ok("tail_log", {"lines": mcp.MAX_LOG_LINES})["lines"]), mcp.MAX_LOG_LINES)
        self.assertEqual(self.ok("tail_log", {"source": "mcp-launch"})["lines"], ["launch output"])
        self.assertEqual(self.ok("tail_log", {"lines": 5, "source": "launcher"})["source"], "launcher")

    def test_tail_log_reads_only_the_end_of_a_huge_file(self):
        log = self.fx.data / "logs/launcher.log"
        with open(log, "wb") as handle:
            handle.write(b"head-marker\n" + b"filler\n" * (mcp.MAX_LOG_READ_BYTES // 7 * 3) + b"tail-marker\n")
        lines = self.ok("tail_log", {"lines": 200})["lines"]
        self.assertEqual(lines[-1], "tail-marker")
        self.assertNotIn("head-marker", lines)

    def test_tail_log_total_size_is_capped(self):
        (self.fx.data / "logs/launcher.log").write_text(("x" * 1900 + "\n") * 200)
        lines = self.ok("tail_log", {"lines": 200})["lines"]
        self.assertLessEqual(sum(map(len, lines)), mcp.MAX_LOG_TOTAL_CHARS)
        self.assertGreater(len(lines), 10)

    def test_settings_file_may_be_a_symlink_to_a_dotfile_repo(self):
        real = self.fx.root / "dotfiles.conf"
        real.write_text("[General]\nprefix=/p\n")
        self.fx.config_file.symlink_to(real)
        self.assertEqual(mcp.read_settings(self.fx.config)["prefix"], "/p")
        real.write_text("x" * (mcp.MAX_SETTINGS_BYTES + 1))
        with self.assertRaises(mcp.ToolError):
            mcp.read_settings(self.fx.config)

    def test_a_fifo_or_other_non_regular_log_is_refused_without_blocking(self):
        os.mkfifo(self.fx.data / "logs/launcher.log")
        started = time.monotonic()
        self.refused("tail_log", {}, "not a regular file")
        self.refused("launcher_status", {}, "not a regular file")
        self.assertLess(time.monotonic() - started, 5)

    def test_tail_log_missing_and_symlinked_logs(self):
        self.assertEqual(self.ok("tail_log")["lines"], [])
        secret = self.fx.root / "secret.txt"
        secret.write_text("top secret\n")
        (self.fx.data / "logs/launcher.log").symlink_to(secret)
        self.refused("tail_log", {}, "cannot open log")
        shutil.rmtree(self.fx.data / "logs")
        (self.fx.data / "logs").symlink_to(self.fx.root)
        self.refused("tail_log", {}, "refusing symlink")


ADVERSARIAL_INPUTS = {
    "userinfo colons": lambda n: "://" + "a:" * (n // 2), "scheme repeated": lambda n: "://x" * (n // 4), "nested userinfo": lambda n: "://a:a" * (n // 6),
    "empty userinfo": lambda n: "://:" * (n // 4), "at signs": lambda n: "@" * n, "at after userinfo": lambda n: "://a:" + "@" * n, "long password": lambda n: "://a:" + "x" * n,
    "percent run": lambda n: "%41" * (n // 3), "percent and userinfo": lambda n: "%41://" + "a:" * (n // 2), "mixed encodings": lambda n: "%2F\\/\uff0f%252F" * (n // 12),
    "encoded secret": lambda n: "password%3D" + "hunter2" * (n // 7), "many labels": lambda n: "password=a token=b secret=c " * (n // 27), "unclosed quotes": lambda n: 'token="a ' * (n // 9),
    "spaced labels": lambda n: "password abc " * (n // 13), "whitespace": lambda n: " " * n, "slash homes": lambda n: "/home/" * (n // 6), "bearer": lambda n: "bearer x " * (n // 9),
    "jwt prefix repeated": lambda n: "eyJ" * (n // 3) + "%25252541", "jwt segments repeated": lambda n: "eyJAAAAAAAAAA." * (n // 14), "sk prefix repeated": lambda n: "sk-" * (n // 3) + "%25252541",
    "bearer repeated": lambda n: "Bearer " * (n // 7) + "%25", "password= repeated": lambda n: "password=" * (n // 9), "scheme chain": lambda n: "://" * (n // 3) + "%25252541",
    "percent chain": lambda n: "%25" * (n // 3) + "41", "ghp repeated": lambda n: "ghp_" * (n // 4) + "%2541", "github_pat repeated": lambda n: "github_pat_" * (n // 11) + "%25",
    "AKIA repeated": lambda n: "AKIA" * (n // 4) + "%25", "userinfo prefix repeated": lambda n: "://a:" * (n // 5) + "@", "userinfo tokens": lambda n: "x://a:b@h " * (n // 10),
    "token prefixes": lambda n: "ghp_" + "A" * n, "jwt": lambda n: "eyJ" + "A" * 20 + "." + "A" * n, "fullwidth": lambda n: "\uff0f" * n, "nfkc expansion": lambda n: "\ufb01" * n}



class RedactionTest(ToolTestCase):
    def test_home_user_and_tokens_are_redacted_everywhere(self):
        fx = self.fx
        token = "ghp_" + "A1b2C3d4E5" * 3
        (fx.data / "logs/launcher.log").write_text(
            f"opened {fx.home}/Documents/file by {USER}\nowner {USER}@host and /home/other/stuff\n"
            f"auth Bearer abcdef123456789 and {token}\npassword=hunter2hunter2 ok\nalicesmith stays\n")
        fx.settings(darling=f"{fx.home}/bin/darling", volume=f"/run/media/{USER}/macOS")
        result = fx.call("tail_log", {"lines": 10})
        blob = json.dumps(result)
        for secret in (str(fx.home), "hunter2", token, "abcdef123456789", "/home/other"):
            self.assertNotIn(secret, blob, secret)
        text = result["content"][0]["text"]
        self.assertIn("opened ~/Documents/file by alice", text)
        self.assertIn("/home/<user>/stuff", text)
        self.assertIn("alicesmith stays", text)
        status = json.dumps(fx.call("launcher_status"))
        self.assertNotIn(str(fx.home), status)
        self.assertNotIn(f"/{USER}/", status)
        self.assertIn("/run/media/<user>/macOS", status)
        self.assertIn("~/bin/darling", status)

    def test_error_messages_are_redacted_too(self):
        result = self.fx.call("list_apps", {"prefix": f"{self.fx.home}/private"})
        self.assertNotIn(str(self.fx.home), json.dumps(result))

    def test_redactor_handles_root_and_short_user_names(self):
        self.assertEqual(mcp.Redactor("/root").text("/root/x and root"), "~/x and root")
        self.assertEqual(mcp.Redactor("/home/a").text("a /home/a"), "a ~")

    def test_encoding_and_case_do_not_defeat_path_redaction(self):
        redactor = mcp.Redactor("/home/alice")
        self.assertEqual(redactor.text("/HOME/bob/x /USERS/carol /Run/Media/dave/v"), "/HOME/<user>/x /USERS/<user> /Run/Media/<user>/v")
        for leaked in ("%2Fhome%2Falice%2Fx", "%2fhome%2falice", "/HOME/alice/x", "/Home/Alice", "%252Fhome%252Falice%252Fx", "/home/alice", "/HOME/ALICE/x"):
            text = redactor.text(f"see {leaked} now")
            self.assertNotRegex(text, r"(?i)alice")
            self.assertTrue(text.startswith("see ") and text.endswith(" now"), text)

    def test_percent_decoded_matches_redact_the_original_and_never_return_decoded_text(self):
        redactor = mcp.Redactor("/home/alice")
        cases = {
            "x %2Fhome%2Falice%2Fdoc y": ("x ~%2Fdoc y", ("/home", "alice")),
            "x %252Fhome%252Falice%252Fdoc y": ("x ~%252Fdoc y", ("/home", "alice", "%2Fhome")),
            "x %2Fhome%2Fbob y": ("x %2Fhome%2F<user> y", ("/home", "bob")),
            "password%3Dhunter2xyz tail": ("password%3D<redacted> tail", ("hunter", "password=")),
            "password%253Dhunter2xyz tail": ("password%253D<redacted> tail", ("hunter", "password=")),
            "pass%77ord=hunter2xyz": ("pass%77ord=<redacted>", ("hunter",)),
            "token%3A%20abc123def": ("token%3A%20<redacted>", ("abc123",)),
            "%255Cx %2Fhome%2Falice": ("%255Cx ~", ("alice", "/home")),
        }
        for text, (expected, absent) in cases.items():
            result = redactor.text(text)
            self.assertEqual(result, expected, text)
            for fragment in absent:
                self.assertNotIn(fragment, result, text)

    def test_mixed_encoded_and_plain_matches_in_one_string_are_all_redacted(self):
        redactor = mcp.Redactor("/home/alice")
        text = "a password%3Dhunter2xyz b password=plainsecret c /home/bob/x d %2Fhome%2Fcarol%2Fy e Bearer tok123456 f %2Fhome%2Falice g token=%2525abc"
        result = redactor.text(text)
        for leaked in ("hunter", "plainsecret", "bob", "carol", "alice", "tok123456", "abc", "/home/c", "%2Fhome%2Fa"):
            self.assertNotIn(leaked, result)
        self.assertEqual(result, "a password%3D<redacted> b password=<redacted> c /home/<user>/x d %2Fhome%2F<user>%2Fy e Bearer <redacted> f ~ g token=<redacted>")

    def test_decoding_goes_to_a_fixed_point_with_a_depth_cap(self):
        redactor = mcp.Redactor("/home/alice")
        secret = "password=hunter2xyz"
        encoded = secret
        for layer in range(1, mcp.Redactor.MAX_DECODE_DEPTH + 1):
            encoded = encoded.replace("%", "%25").replace("=", "%3D") if layer == 1 else encoded.replace("%", "%25")
            self.assertNotIn("hunter", redactor.text(encoded), f"layer {layer}")
            self.assertNotIn("hunter", redactor.text("pre " + encoded + " post"), f"layer {layer}")

    def test_a_secret_split_across_decode_layers_is_not_half_returned(self):
        redactor = mcp.Redactor("/home/alice")
        for text in ("%2Fhome%2Fal%252Fice%2Fx", "token=abc%2525def", "password=a%2Fb%252Fc"):
            result = redactor.text(text)
            self.assertNotRegex(result, r"ice|abc|def|a%2Fb|%252Fc", text)

    def test_whitespace_separated_labelled_secrets_are_redacted(self):
        redactor = mcp.Redactor("/home/alice")
        for text, secret in (("password hunter2xyz", "hunter2"), ("token abcdef123456", "abcdef"), ("Authorization Basic dXNlcjpwYXNz", "dXNlcjpw"),
                             ("authorization Bearer abc.def.ghi", "abc.def"), ("Bearer abcdef123", "abcdef"), ("secret   s3cr3tvalue", "s3cr3t"),
                             ("API-KEY abcd1234efgh", "abcd1234"), ("passwd  longerpass", "longerpass"), ("PASSWORD\thunter2xyz", "hunter2"), ("access_key AKIA0000", "AKIA0000")):
            result = redactor.text(f"before {text} after")
            self.assertNotIn(secret, result, text)
            self.assertTrue(result.startswith("before ") and result.endswith(" after"), result)

    def test_ordinary_prose_with_the_labels_is_not_mangled_beyond_one_following_word(self):
        redactor = mcp.Redactor("/home/alice")
        untouched = ("the password field is empty", "This token is invalid", "tokens and passwords are listed", "Please enter your password to continue", "a secret to everybody",
                     "no authorization yet", "password: ", "set the token later")
        for plain in untouched:
            self.assertEqual(redactor.text(plain), plain)
        self.assertEqual(redactor.text("the password confirmation mismatch was shown"), "the password <redacted> mismatch was shown")
        self.assertEqual(redactor.text("token expired yesterday, token fine"), "token <redacted> yesterday, token fine")

    def test_redaction_is_idempotent(self):
        redactor = mcp.Redactor("/home/alice")
        once = redactor.text("password=hunter2xyz /home/bob/x %2Fhome%2Fcarol Bearer abcdef123 password confirmation")
        self.assertEqual(redactor.text(once), once)

    def test_labelled_secrets_are_redacted_whole_whatever_they_contain(self):
        redactor = mcp.Redactor("/home/alice")
        for secret_text in ("password=ab!cdefgh", "PASSWORD: s3cr3t!!", "Authorization: Basic dXNlcjpwYXNzd29yZA==", "authorization=Bearer abc.def", "token=a b",
                            'token: "two words"', "{\"password\": \"hunter 2\"}", "api_key=AbC/+=", "Secret = 'x y z'", "passwd=ab", "password%3Dhunter2xyz",
                            "basic dXNlcjpwYXNz", "BEARER abcdef12345", "x-api-key: abc123def"):
            text = redactor.text(f"before {secret_text} after")
            self.assertIn("<redacted>", text, secret_text)
            for fragment in ("ab!cdefgh", "s3cr3t", "dXNlcjpw", "abc.def", "two words", "hunter", "AbC/+=", "x y z", "ab ", "abcdef12345", "abc123def"):
                self.assertNotIn(fragment, text, secret_text)

    def test_ordinary_text_is_left_alone(self):
        redactor = mcp.Redactor("/home/alice")
        for plain in ("100% done", "tokens are fine", "the password field", "/dev/sda1 mounted", "path /home/ and 50%2 left", "basically fine"):
            self.assertEqual(redactor.text(plain), plain)

    def test_harmless_percent_sequences_stay_verbatim_and_decoded_text_is_never_returned(self):
        redactor = mcp.Redactor("/home/alice")
        for plain in ("a%41b", "100%41 done", "%2F", "%7Bx%7D", "%41%42%43", "50%", "%zz %4"):
            self.assertEqual(redactor.text(plain), plain)
        self.assertEqual(redactor.text("x %41 y %2Fhome%2Falice"), "x %41 y ~")
        self.assertEqual(redactor.text("%41 password%3Dhunter2xyz %42"), "%41 password%3D<redacted> %42")

    def test_homes_outside_slash_home_match_in_any_case(self):
        redactor = mcp.Redactor("/srv/Alice")
        self.assertEqual(redactor.text("/SRV/alice/x /srv/ALICE /srv/Alicex"), "~/x ~ /srv/Alicex")

    def test_separators_next_to_a_home_or_user_path_survive_redaction(self):
        redactor = mcp.Redactor("/home/alice")
        cases = {"PATH=/home/alice/bin:/home/alice:/usr/bin": "PATH=~/bin:~:/usr/bin", "(/home/alice)": "(~)", "/home/bob:/x": "/home/<user>:/x",
                 "[/home/bob]": "[/home/<user>]", "a,/home/bob,b": "a,/home/<user>,b", "/home/bob;ls": "/home/<user>;ls", "x=/Users/bob|y": "x=/Users/<user>|y",
                 "<a href=/home/bob>": "<a href=/home/<user>>", "/home/alice:/home/bob:/home/carol": "~:/home/<user>:/home/<user>", "{\"p\":\"/home/bob\"}": "{\"p\":\"/home/<user>\"}"}
        for text, expected in cases.items():
            self.assertEqual(redactor.text(text), expected, text)

    def test_overlapping_paths_do_not_widen_into_a_union_but_secrets_still_do(self):
        redactor = mcp.Redactor("/home/alice")
        self.assertEqual(redactor.text("/home/alice/x"), "~/x")
        self.assertEqual(redactor.text("password=/home/alice/x tail"), "password=<redacted> tail")
        partial = mcp.Redactor("/home/bob").text("see /home/bob@corp now")
        self.assertEqual(partial, "see ~<user> now")  # the home span ends where the user-name span runs on: no union

    def test_url_userinfo_is_redacted(self):
        redactor = mcp.Redactor("/home/alice")
        for url in ("https://bob:hunter22secret@host/x", "git clone ssh://u:p%40ss@h.example:22/r", "postgres://app:s3cr3tpw@db:5432/x"):
            result = redactor.text(url)
            self.assertIn("://<redacted>@", result)
            for leaked in ("hunter22", "bob:", "p%40ss", "s3cr3t", "app:"):
                self.assertNotIn(leaked, result)
        self.assertEqual(redactor.text("https://example.com/a@b and mailto:x@y"), "https://example.com/a@b and mailto:x@y")

    def test_userinfo_with_at_signs_slashes_and_empty_users_is_fully_redacted(self):
        redactor = mcp.Redactor("/home/alice")
        cases = {"https://bob:p@ss@host": "https://<redacted>@host", "https://bob:pa/ss@host": "https://<redacted>@host", "redis://:secretpw@host": "redis://<redacted>@host",
                 "see https://bob:p@ss@host/x now": "see https://<redacted>@host/x now", "http://host:8080@x": "http://<redacted>@x", "amqp://u:@h": "amqp://<redacted>@h"}
        for text, expected in cases.items():
            self.assertEqual(redactor.text(text), expected, text)
            for leaked in ("ss@", "pa/", "secretpw"):
                self.assertNotIn(leaked, redactor.text(text))

    def test_credentials_of_any_length_are_redacted_whole(self):
        redactor = mcp.Redactor("/home/alice")
        self.assertEqual(redactor.text("https://" + "u" * 300 + ":" + "p" * 2000 + "@host/x"), "https://<redacted>@host/x")
        self.assertEqual(redactor.text("x https://bob:" + "pw/@" * 400 + "host y"), "x https://<redacted>@host y")
        self.assertEqual(redactor.text("a://u:p@h b://v:q@i"), "a://<redacted>@h b://<redacted>@i")

    def test_a_credential_after_an_earlier_scheme_in_the_same_token_is_found(self):
        redactor = mcp.Redactor("/home/alice")
        self.assertEqual(redactor.text("http://h/x?u=https://bob:pw@host"), "http://h/x?u=https://<redacted>@host")
        self.assertEqual(redactor.text("a://b/c://d://u:p@h"), "a://b/c://<redacted>@h")

    def test_documented_limits_of_the_userinfo_scan(self):
        redactor = mcp.Redactor("/home/alice")
        self.assertEqual(redactor.text("https://bob:8080/x@y"), "https://bob:8080/x@y")  # a numeric password followed by '/' reads as a port
        self.assertEqual(redactor.text("https://bob:123/ss@host"), "https://bob:123/ss@host")
        self.assertEqual(redactor.text("https://bob:pw@host/path@x"), "https://<redacted>@x")  # the last '@' of the token ends the credentials
        self.assertEqual(redactor.text("https://bob:pw @host"), "https://bob:pw @host")  # whitespace ends the token

    def test_urls_without_credentials_are_left_alone(self):
        redactor = mcp.Redactor("/home/alice")
        for plain in ("http://[::1]:8080/path@x", "http://[2001:db8::1]:80/", "http://host:8080/user@x", "https://example.com:443/a@b", "git@github.com:x/y.git", "ssh://git@host",
                      "ssh://git@host:22/repo", "postgres://host:5432/db", "http://localhost:3000", "mailto:a@b", "https://user@host:80/p@x", "https://example.com:443 and a@b", "file:///etc/x@y",
                      "http://example.com/a:b@c", "https://example.com/path/x:y@z"):
            self.assertEqual(redactor.text(plain), plain)

    def isolated(self, name, size, capped, limit_mb=1024):
        """Redacts one adversarial input in a child with an address-space limit and a deadline, so a broken cap or a
        non-linear pattern fails this test quickly instead of eating the machine's memory or time."""
        script = ("import resource, sys, time, importlib.util\n"
                  f"resource.setrlimit(resource.RLIMIT_AS, ({limit_mb} << 20, {limit_mb} << 20))\n"
                  "spec = importlib.util.spec_from_file_location('t', sys.argv[1]); t = importlib.util.module_from_spec(spec); spec.loader.exec_module(t)\n"
                  "if sys.argv[4] == 'uncapped': t.mcp.Redactor.MAX_SCAN_CHARS = 10 ** 9\n"
                  "text = t.ADVERSARIAL_INPUTS[sys.argv[2]](int(sys.argv[3]))\n"
                  "started = time.monotonic(); t.mcp.Redactor('/home/alice').text(text); elapsed = time.monotonic() - started\n"
                  "print(elapsed, resource.getrusage(resource.RUSAGE_SELF).ru_maxrss // 1024)\n")
        try:
            done = subprocess.run([sys.executable, "-I", "-c", script, str(Path(__file__).resolve()), name, str(size), "capped" if capped else "uncapped"],
                                  capture_output=True, text=True, timeout=30)
        except subprocess.TimeoutExpired:
            self.fail(f"{name} at {size} did not finish within 30 s")
        self.assertEqual(done.returncode, 0, f"{name} at {size}: {done.stderr[-300:]}")
        elapsed, peak = (float(x) for x in done.stdout.split())
        return elapsed, peak

    def test_adversarial_input_cannot_stall_the_redactor_or_balloon_its_memory(self):
        for size in (65536, 1048576):
            for name in ADVERSARIAL_INPUTS:
                elapsed, peak = self.isolated(name, size, capped=True)
                self.assertLess(elapsed, 1.0, f"{name} at {size} took {elapsed:.2f}s")
                self.assertLess(peak, 400, f"{name} at {size} peaked at {peak:.0f} MB")

    def test_the_patterns_alone_are_linear_without_the_input_cap(self):
        for name in ADVERSARIAL_INPUTS:
            elapsed, peak = self.isolated(name, 131072, capped=False)
            self.assertLess(elapsed, 1.5, name)
            self.assertLess(peak, 400, name)

    STRADDLERS = {
        "labelled": ("password=hunter2xyz", ("hunt", "xyz")), "spaced label": ("password hunter2xyz", ("hunt", "xyz")),
        "quoted value": ('password="hunter2 xyz"', ("hunt", "xyz")), "unclosed quote": ('password="hunter2 xyz', ("hunt", "xyz")),
        "userinfo": ("https://bob:p@ss@host/x", ("bob", "p@ss", "ss@")), "home path": ("/home/bobby/docs", ("bo",)),
        "encoded home": ("%2Fhome%2Fbobby%2Fdocs", ("bo",)), "encoded label": ("password%3Dhunter2xyz", ("hunt", "xyz")),
        "github token": ("ghp_" + "A1b2C3d4E5" * 3, ("ghp_", "A1b2")), "jwt": ("eyJ" + "Q" * 20 + "." + "R" * 20 + "." + "S" * 20, ("eyJ", "QQQQ", "RRRR")),
        "userinfo slash password": ("https://bob:ab/" + "QZ" * 60 + "@host", ("bob", "ab/", "QZ")),
        "long userinfo with slashes": ("https://bob:" + "pw/" * 200 + "@h", ("bob", "pw/")),
        "long label value": ("password=" + "h" * 800, ("hh",)), "long userinfo": ("https://u:" + "p" * 400 + "@h", ("ppp",)),
        "encoded bearer": ("Bearer%20abcdef123456", ("abcdef",))}
    FILLERS = {"words": "x ", "no whitespace": "A", "delimiters": "a/b,", "newlines": "x\n"}

    def test_a_secret_straddling_the_truncation_limit_never_shows_a_partial_prefix(self):
        redactor = mcp.Redactor("/home/alice")
        limit, lookahead = 1200, 300
        with patch.object(mcp.Redactor, "MAX_SCAN_CHARS", limit), patch.object(mcp.Redactor, "LOOKAHEAD_CHARS", lookahead):
            for name, (secret, fragments) in self.STRADDLERS.items():
                starts = sorted(set(range(limit - len(secret) - 10, limit + 12)) | set(range(max(0, limit - len(secret) - 400), limit + 12, 37)))
                for filler_name, unit in self.FILLERS.items():
                    for start in starts:
                        prefix = (unit * (start // len(unit) + 1))[:start]
                        text = prefix + "," + secret + ", " + (unit * 400)
                        text += unit * ((limit + lookahead + 150 - len(text)) // len(unit) + 1)
                        self.assertGreater(len(text), limit + lookahead)
                        result = redactor.text(text)
                        where = f"{name} / {filler_name} / start {start}"
                        self.assertTrue(result.endswith(mcp.Redactor.TRUNCATION_MARKER), where)
                        self.assertLessEqual(len(result), limit + len(mcp.Redactor.TRUNCATION_MARKER) + 16, where)
                        for fragment in fragments:
                            self.assertNotIn(fragment, result.replace("<redacted>", "").replace("<user>", ""), where)

    def test_text_between_the_limit_and_the_window_end_is_still_redacted_not_emitted_raw(self):
        redactor = mcp.Redactor("/home/alice")
        limit, lookahead = 1000, 200
        with patch.object(mcp.Redactor, "MAX_SCAN_CHARS", limit), patch.object(mcp.Redactor, "LOOKAHEAD_CHARS", lookahead):
            text = "w " * 480 + "password=hunter2xyz " + "w " * 400
            self.assertEqual(redactor.text(text).count("hunter"), 0)
            self.assertIn("password=<redacted>", redactor.text(text))
            beyond = "w " * 520 + "password=hunter2xyz " + "w " * 400
            self.assertNotIn("password", redactor.text(beyond))

    def test_oversized_text_is_cut_after_redaction_and_stays_bounded(self):
        redactor = mcp.Redactor("/home/alice")
        limit = mcp.Redactor.MAX_SCAN_CHARS
        short = "password hunter2xyz and /home/alice/x"
        self.assertEqual(redactor.text(short), "password <redacted> and ~/x")
        text = "word " * (limit // 5 + 10) + "password hunter2xyz " + "tail " * 2000
        result = redactor.text(text)
        self.assertTrue(result.endswith(mcp.Redactor.TRUNCATION_MARKER))
        self.assertLessEqual(len(result), limit + len(mcp.Redactor.TRUNCATION_MARKER))
        self.assertNotIn("hunter", result)
        self.assertNotIn("tail", result)
        at_the_limit = "x " * (limit // 2 - 5) + "password=hunter2xyz " + "y " * 5000
        straddling = redactor.text(at_the_limit)
        self.assertNotIn("hunt", straddling)
        self.assertEqual(redactor.text("A" * (limit + mcp.Redactor.LOOKAHEAD_CHARS + 5000)), mcp.Redactor.TRUNCATION_MARKER)

    def test_the_cap_applies_to_every_string_in_a_result(self):
        (self.fx.data / "logs/launcher.log").write_text(("token=abc " + "z" * 1900 + "\n") * 5)
        result = self.fx.call("tail_log", {"lines": 5})
        self.assertFalse(result["isError"])
        self.assertNotIn("token=abc", json.dumps(result))
        self.fx.settings(darling="x" * 300000)
        started = time.monotonic()
        status = self.fx.call("launcher_status")
        self.assertLess(time.monotonic() - started, 2.0)
        self.assertLessEqual(len(status["structuredContent"]["selected"]["darling"]), mcp.Redactor.MAX_SCAN_CHARS + 100)

    def test_json_escaped_slashes_and_unicode_escapes_are_found_without_decoding_the_output(self):
        redactor = mcp.Redactor("/home/alice")
        self.assertEqual(redactor.text("\\/home\\/alice\\/x"), "~\\/x")
        self.assertEqual(redactor.text('{"p": "\\/home\\/bob\\/y"}'), '{"p": "\\/home\\/<user>\\/y"}')
        self.assertEqual(redactor.text("\\u002fhome\\u002fbob"), "\\u002fhome\\u002f<user>")
        self.assertEqual(redactor.text("password\\u003dhunter2xyz"), "password\\u003d<redacted>")
        self.assertEqual(redactor.text("a\\nb \\d \\u00e9"), "a\\nb \\d \\u00e9")

    def test_fullwidth_and_other_compatibility_characters_are_folded_for_matching_only(self):
        redactor = mcp.Redactor("/home/alice")
        self.assertEqual(redactor.text("\uff0fhome\uff0falice\uff0fx"), "~\uff0fx")
        self.assertEqual(redactor.text("\uff0fhome\uff0fbob"), "\uff0fhome\uff0f<user>")
        self.assertEqual(redactor.text("\uff50\uff41\uff53\uff53\uff57\uff4f\uff52\uff44=hunter2xyz"), "\uff50\uff41\uff53\uff53\uff57\uff4f\uff52\uff44=<redacted>")
        self.assertEqual(redactor.text("caf\u00e9 \ufb01le \u00fc"), "caf\u00e9 \ufb01le \u00fc")

    def test_a_user_name_is_only_redacted_as_a_home_path_component(self):
        redactor = mcp.Redactor("/home/dev")
        self.assertEqual(redactor.text("/dev/sda1 mounted by dev at /home/dev/x and /home/other/y"), "/dev/sda1 mounted by dev at ~/x and /home/<user>/y")
        self.assertEqual(mcp.Redactor("/home/darling").text("darling exec /usr/bin/darling in /home/darling"), "darling exec /usr/bin/darling in ~")
        self.assertEqual(mcp.Redactor("/Users/me").text("/Users/me/Library /Users/you/Library"), "~/Library /Users/<user>/Library")


FAKE_DARLING = """touch "$(dirname "$0")/ran"
case "$1" in
  exec)
    echo "exec $2 prefix=$DPREFIX install=$DARLING_INSTALL_PREFIX leak_a=$GITHUB_TOKEN leak_b=$DYLD_LIBRARY_PATH"
    case "$2" in *Slow*) sleep 2; echo slow-late;; *Fail*) echo "boom" >&2; exit 7;; *Big*) head -c 300000 /dev/zero | tr '\\0' x;; esac ;;
  shutdown)
    echo "shutdown prefix=$DPREFIX"
    case "$DPREFIX" in *hang*) sleep 1; touch "$(dirname "$0")/finished";; *broken*) echo "no such prefix" >&2; exit 4;; *big*) head -c 300000 /dev/zero | tr '\\0' x;; esac ;;
esac
"""


FAKE_LAUNCHER = """echo "argv: $*"
case "$*" in *broken*) echo "no /usr/share/langid" >&2; exit 1;; esac
"""


class ActionToolsTest(unittest.TestCase):
    def setUp(self):
        self.fx = Fixture(allow_actions=True)
        self.addCleanup(self.fx.close)
        self.darling = self.fx.executable("darling", FAKE_DARLING)
        self.fx.settings(darling=self.darling)
        self.fx.prefix("default", [("Calc", "Calculator"), ("Slow", "Slow"), ("Fail", "Fail")])

    def ran(self):
        return (self.fx.bin / "ran").exists()

    def ok(self, name, arguments):
        result = self.fx.call(name, arguments)
        self.assertFalse(result.get("isError"), result)
        return result["structuredContent"]

    def refused(self, name, arguments, fragment):
        result = self.fx.call(name, arguments)
        self.assertTrue(result.get("isError"), result)
        self.assertIn(fragment, result["content"][0]["text"])

    def test_actions_are_refused_without_the_environment_opt_in(self):
        fx = Fixture(allow_actions=False)
        self.addCleanup(fx.close)
        fx.executable("darling", FAKE_DARLING)
        fx.settings(darling=fx.bin / "darling")
        fx.prefix("default", [("Calc", "Calculator")])
        for name, arguments in (("launch_app", {"prefix": "default", "app": "Calc", "confirm": True}), ("stop_prefix", {"prefix": "default", "confirm": True}),
                                ("import_langid_data", {"prefix": "default", "volume": self.volume(), "confirm": True})):
            result = fx.call(name, arguments)
            self.assertTrue(result["isError"])
            self.assertIn("DARLING_LAUNCHER_MCP_ALLOW_ACTIONS=1", result["content"][0]["text"])
        self.assertFalse((fx.bin / "ran").exists())

    def test_actions_are_refused_without_confirm_true(self):
        for confirm in (False, "true", 1, None):
            self.refused("launch_app", {"prefix": "default", "app": "Calc", "confirm": confirm}, "confirm")
            self.refused("stop_prefix", {"prefix": "default", "confirm": confirm}, "confirm")
        self.refused("stop_prefix", {"prefix": "default"}, "missing required argument 'confirm'")
        self.assertFalse(self.ran())

    def test_refusals_come_before_any_prefix_lookup(self):
        self.refused("stop_prefix", {"prefix": "../../etc", "confirm": False}, "confirm must be true")

    def test_launch_runs_darling_exec_with_prefix_and_clean_environment(self):
        runtime = self.fx.root / "runtime"
        runtime.mkdir()
        self.fx.settings(darling=self.darling, runtimeRoot=runtime)
        with patch.dict(os.environ, {"GITHUB_TOKEN": "sekrit-token-value", "DYLD_LIBRARY_PATH": "/evil"}):
            data = self.ok("launch_app", {"prefix": "default", "app": "Calc", "confirm": True})
        self.assertEqual(data["state"], "exited")
        self.assertEqual(data["exit_code"], 0)
        prefix = self.fx.data / "prefixes/default"
        self.assertIn("exec /Applications/Calc.app/Contents/MacOS/Calculator prefix=~/.darling-launcher/prefixes/default install=", data["output"])
        self.assertIn("/runtime leak_a= leak_b=", data["output"])
        self.assertNotIn("sekrit", json.dumps(data))
        self.assertEqual(self.ok("launch_app", {"prefix": str(prefix), "app": "Applications/Calc.app", "confirm": True, "wait_seconds": 0})["bundle"], "Applications/Calc.app")

    def test_launch_without_runtime_root_leaves_install_prefix_unset(self):
        data = self.ok("launch_app", {"prefix": "default", "app": "Calc", "confirm": True})
        self.assertIn("install= leak_a=", data["output"])

    def test_launch_output_is_logged_and_nonzero_exit_reported(self):
        data = self.ok("launch_app", {"prefix": "default", "app": "Fail", "confirm": True})
        self.assertEqual(data["exit_code"], 7)
        self.assertIn("boom", data["output"])
        lines = self.ok("tail_log", {"source": "mcp-launch"})["lines"]
        self.assertTrue(any("boom" in line for line in lines))
        self.assertTrue(any("[launch:Fail]" in line for line in lines))

    def test_each_launch_has_its_own_log_and_never_shows_another_apps_output(self):
        prefix = self.fx.data / "prefixes/default"
        (prefix / "Applications/Slow.app/Contents/MacOS").mkdir(parents=True, exist_ok=True)
        slow = self.ok("launch_app", {"prefix": "default", "app": "Slow", "confirm": True, "wait_seconds": 0})
        calc = self.ok("launch_app", {"prefix": "default", "app": "Calc", "confirm": True})
        self.assertNotEqual(slow["log_file"], calc["log_file"])
        self.assertNotIn("Slow", calc["output"].split("\n", 1)[1])
        self.fx.server.children[0].wait(timeout=10)
        files = sorted(os.listdir(self.fx.data / "logs/mcp-launch"))
        self.assertEqual(len(files), 2)
        contents = {name: (self.fx.data / "logs/mcp-launch" / name).read_text() for name in files}
        self.assertIn("slow-late", contents[slow["log_file"]])
        self.assertNotIn("slow-late", contents[calc["log_file"]])
        self.assertNotIn("Calculator", contents[slow["log_file"]])
        self.assertEqual(oct(os.stat(self.fx.data / "logs/mcp-launch" / calc["log_file"]).st_mode & 0o777), "0o600")

    def launch_logs_with_ages(self, ages):
        directory = self.fx.data / "logs/mcp-launch"
        directory.mkdir(parents=True)
        for index, age in enumerate(ages):
            path = directory / f"20200101T0000{index:02d}-{index:08x}.log"
            path.write_text("old\n")
            os.utime(path, (time.time() - age, time.time() - age))
        return directory

    def test_only_old_logs_beyond_the_newest_are_pruned(self):
        directory = self.launch_logs_with_ages([7200] * 25)
        (directory / "keep-me.txt").write_text("not ours")
        data = self.ok("launch_app", {"prefix": "default", "app": "Calc", "confirm": True})
        names = sorted(n for n in os.listdir(directory) if n.endswith(".log"))
        self.assertEqual(len(names), mcp.MAX_LAUNCH_LOGS)
        self.assertEqual(names[-1], data["log_file"])
        self.assertTrue((directory / "keep-me.txt").exists())
        self.assertNotIn("20200101T000000-00000000.log", names)

    def test_logs_written_within_the_hour_are_never_pruned_even_beyond_the_limit(self):
        directory = self.launch_logs_with_ages([10] * 25)
        self.ok("launch_app", {"prefix": "default", "app": "Calc", "confirm": True})
        self.assertEqual(len([n for n in os.listdir(directory) if n.endswith(".log")]), 26)

    def test_mixed_ages_delete_exactly_the_old_ones_outside_the_newest_twenty(self):
        directory = self.launch_logs_with_ages([7200] * 3 + [10] * 3 + [7200] * 19)
        self.ok("launch_app", {"prefix": "default", "app": "Calc", "confirm": True})
        names = sorted(n for n in os.listdir(directory) if n.endswith(".log"))
        self.assertEqual(len(names), 26 - 3)
        self.assertNotIn("20200101T000000-00000000.log", names)
        self.assertIn("20200101T000003-00000003.log", names)

    def test_an_existing_file_or_symlink_at_the_log_name_is_never_followed_or_appended_to(self):
        directory = self.fx.data / "logs/mcp-launch"
        directory.mkdir(parents=True)
        victim = self.fx.root / "victim.txt"
        victim.write_text("precious\n")
        real_strftime = time.strftime
        fixed = lambda fmt, *a: "20260101T000000" if fmt == "%Y%m%dT%H%M%S" else real_strftime(fmt, *a)
        with patch.object(mcp.secrets, "token_hex", lambda n: "beef"), patch.object(mcp.time, "strftime", fixed):
            target = directory / "20260101T000000-0000beef.log"
            target.symlink_to(victim)
            self.refused("launch_app", {"prefix": "default", "app": "Calc", "confirm": True}, "cannot create the launch log")
            target = directory / "20260101T000000-0001beef.log"  # the frozen clock makes the next launch use the next sequence number
            target.write_text("already here\n")
            self.refused("launch_app", {"prefix": "default", "app": "Calc", "confirm": True}, "cannot create the launch log")
        self.assertEqual(victim.read_text(), "precious\n")
        self.assertEqual(target.read_text(), "already here\n")
        self.assertFalse(self.ran())

    def with_clock(self, start):
        """A controllable clock for the time functions the server uses; the local zone is CET/CEST (clocks go back at 03:00 CEST on 2026-10-25)."""
        clock = [float(start)]
        real = {"gmtime": time.gmtime, "localtime": time.localtime, "strftime": time.strftime}
        fake_localtime = lambda secs=None: real["localtime"](clock[0] if secs is None else secs)
        patches = [patch.dict(os.environ, {"TZ": "CET-1CEST,M3.5.0,M10.5.0/3"}),
                   patch.object(time, "gmtime", lambda secs=None: real["gmtime"](clock[0] if secs is None else secs)),
                   patch.object(time, "localtime", fake_localtime),
                   patch.object(time, "strftime", lambda fmt, t=None: real["strftime"](fmt, fake_localtime() if t is None else t))]
        for item in patches:
            item.start()
            self.addCleanup(item.stop)
        self.addCleanup(time.tzset)
        time.tzset()
        return clock

    def test_launch_log_names_stay_in_creation_order_across_a_dst_fall_back(self):
        clock = self.with_clock(calendar.timegm((2026, 10, 25, 0, 50, 0)))  # 02:50 CEST, the hour before the clocks go back
        self.assertEqual(time.strftime("%H:%M", time.localtime()), "02:50")
        names, apps = [], ["Calc", "Fail", "Calc", "Fail"]
        for delta, app in zip((0, 1200, 2400, 7200), apps):  # 02:10 CET (before 02:50 in local time), 02:30 CET, 03:50 CET
            clock[0] = calendar.timegm((2026, 10, 25, 0, 50, 0)) + delta
            names.append(self.ok("launch_app", {"prefix": "default", "app": app, "confirm": True})["log_file"])
        self.assertEqual(time.strftime("%H:%M", time.localtime()), "03:50")
        self.assertEqual(names, sorted(names))
        self.assertEqual(len(set(names)), 4)
        self.assertTrue(names[0].startswith("20261025T005000-") and names[1].startswith("20261025T011000-"))
        self.assertIn("boom", "\n".join(self.ok("tail_log", {"source": "mcp-launch"})["lines"]))
        clock[0] += 5
        self.ok("launch_app", {"prefix": "default", "app": "Calc", "confirm": True})
        self.assertNotIn("boom", "\n".join(self.ok("tail_log", {"source": "mcp-launch"})["lines"]))
        self.assertTrue(all(mcp.LAUNCH_LOG_RE.match(n) for n in os.listdir(self.fx.data / "logs/mcp-launch")))

    def test_launch_log_names_are_ordered_within_one_second_and_when_the_clock_steps_back(self):
        clock = self.with_clock(calendar.timegm((2026, 6, 1, 12, 0, 0)))
        names = [self.ok("launch_app", {"prefix": "default", "app": "Calc", "confirm": True})["log_file"] for _ in range(3)]
        self.assertEqual(len({n[:15] for n in names}), 1)
        self.assertEqual(names, sorted(names))
        self.assertEqual([n[16:20] for n in names], ["0000", "0001", "0002"])
        clock[0] -= 30  # NTP steps the clock back
        names.append(self.ok("launch_app", {"prefix": "default", "app": "Fail", "confirm": True})["log_file"])
        clock[0] += 31
        names.append(self.ok("launch_app", {"prefix": "default", "app": "Calc", "confirm": True})["log_file"])
        self.assertEqual(names, sorted(names))
        self.assertEqual(len(set(names)), 5)

    def test_a_symlinked_launch_log_directory_is_refused_before_anything_starts(self):
        outside = self.fx.root / "elsewhere"
        outside.mkdir()
        (self.fx.data / "logs").mkdir(exist_ok=True)
        (self.fx.data / "logs/mcp-launch").symlink_to(outside)
        self.refused("launch_app", {"prefix": "default", "app": "Calc", "confirm": True}, "refusing symlink")
        self.assertEqual(os.listdir(outside), [])
        self.assertFalse(self.ran())

    def test_symlinked_and_foreign_files_in_the_launch_log_directory_are_ignored(self):
        directory = self.fx.data / "logs/mcp-launch"
        directory.mkdir(parents=True)
        secret = self.fx.root / "secret.txt"
        secret.write_text("top secret\n")
        (directory / "29990101T000000-deadbeef.log").symlink_to(secret)
        (directory / "20260101T000000-aaaaaaaa.log").write_text("real\n")
        self.assertEqual(self.ok("tail_log", {"source": "mcp-launch"})["lines"], ["real"])

    def test_child_output_is_capped_for_launch_and_stop(self):
        prefix = self.fx.data / "prefixes/default"
        (prefix / "Applications/Big.app/Contents/MacOS").mkdir(parents=True)
        (prefix / "Applications/Big.app/Contents/MacOS/Big").write_text("")
        records = json.loads((prefix / ".darling-launcher/catalog.json").read_text())["apps"]
        records.append({"name": "Big", "bundle": "Applications/Big.app", "executable": "Big"})
        (prefix / ".darling-launcher/catalog.json").write_text(json.dumps({"apps": records}))
        launch = self.ok("launch_app", {"prefix": "default", "app": "Big", "confirm": True})
        self.assertEqual(launch["state"], "exited")
        self.assertLessEqual(len(launch["output"]), mcp.MAX_CHILD_OUTPUT)
        self.assertGreater(len(launch["output"]), mcp.MAX_CHILD_OUTPUT - 1000)
        self.fx.prefix("big")
        stop = self.ok("stop_prefix", {"prefix": "big", "confirm": True})
        self.assertIs(stop["truncated"], True)
        self.assertLessEqual(len(stop["output"]), mcp.MAX_CHILD_OUTPUT)
        self.assertGreater(len(stop["output"]), mcp.MAX_CHILD_OUTPUT - 1000)
        small = self.ok("stop_prefix", {"prefix": "default", "confirm": True})
        self.assertIs(small["truncated"], False)

    def test_a_running_app_is_reported_and_not_killed(self):
        data = self.ok("launch_app", {"prefix": "default", "app": "Slow", "confirm": True, "wait_seconds": 0})
        self.assertEqual(data["state"], "running")
        self.assertIsNone(data["exit_code"])
        os.kill(data["pid"], 0)
        self.assertEqual(len(self.fx.server.children), 1)
        self.fx.server.children[0].wait(timeout=10)
        self.fx.server.reap_children()
        self.assertEqual(self.fx.server.children, [])

    def test_launch_argument_and_app_errors(self):
        base = {"prefix": "default", "confirm": True}
        self.refused("launch_app", {**base, "app": "Nope"}, "no imported app")
        self.refused("launch_app", {**base, "app": "Calc", "wait_seconds": 61}, "between 0 and 60")
        self.refused("launch_app", {**base, "app": "Calc", "extra": 1}, "unknown argument")
        self.refused("launch_app", {"prefix": "../x", "app": "Calc", "confirm": True}, "prefix")
        self.assertFalse(self.ran())

    def test_ambiguous_app_names_require_a_bundle_path(self):
        prefix = self.fx.data / "prefixes/default"
        records = json.loads((prefix / ".darling-launcher/catalog.json").read_text())["apps"]
        (prefix / "Applications/Calc2.app/Contents/MacOS").mkdir(parents=True)
        (prefix / "Applications/Calc2.app/Contents/MacOS/Calculator").write_text("")
        records.append({"name": "Calc", "bundle": "Applications/Calc2.app", "executable": "Calculator"})
        (prefix / ".darling-launcher/catalog.json").write_text(json.dumps({"apps": records}))
        self.refused("launch_app", {"prefix": "default", "app": "Calc", "confirm": True}, "Applications/Calc2.app")
        self.assertEqual(self.ok("launch_app", {"prefix": "default", "app": "Applications/Calc2.app", "confirm": True})["app"], "Calc")

    def test_bundle_paths_cannot_escape_through_symlinks(self):
        prefix = self.fx.data / "prefixes/default"
        outside = self.fx.root / "outside"
        (outside / "Contents/MacOS").mkdir(parents=True)
        (outside / "Contents/MacOS/Calculator").write_text("")
        shutil.rmtree(prefix / "Applications/Calc.app")
        (prefix / "Applications/Calc.app").symlink_to(outside)
        self.refused("launch_app", {"prefix": "default", "app": "Calc", "confirm": True}, "refusing symlink")
        shutil.rmtree(outside / "Contents/MacOS")
        (prefix / "Applications/Calc.app").unlink()
        (prefix / "Applications/Calc.app/Contents/MacOS").mkdir(parents=True)
        (prefix / "Applications/Calc.app/Contents/MacOS/Calculator").symlink_to("/bin/sh")
        self.refused("launch_app", {"prefix": "default", "app": "Calc", "confirm": True}, "refusing symlink")
        (prefix / "Applications/Calc.app/Contents/MacOS/Calculator").unlink()
        self.refused("launch_app", {"prefix": "default", "app": "Calc", "confirm": True}, "missing from the prefix")
        self.assertFalse(self.ran())

    def test_darling_resolution_errors(self):
        args = {"prefix": "default", "app": "Calc", "confirm": True}
        self.fx.settings(darling="relative/darling")
        self.refused("launch_app", args, "absolute path to an executable")
        self.fx.settings(darling=self.fx.root / "missing")
        self.refused("launch_app", args, "absolute path to an executable")
        self.darling.chmod(0o644)
        self.fx.settings(darling=self.darling)
        self.refused("launch_app", args, "absolute path to an executable")
        self.darling.chmod(0o755)
        self.fx.settings(darling=self.darling, runtimeRoot=self.fx.root / "no-such-runtime")
        self.refused("launch_app", args, "runtime install root")
        self.fx.settings()
        with patch.object(mcp, "find_tool", return_value=None):
            self.refused("launch_app", args, "no darling executable")
        self.assertFalse(self.ran())

    def test_an_oversized_settings_value_in_an_error_message_is_capped_and_redacted(self):
        args = {"prefix": "default", "app": "Calc", "confirm": True}
        for value in ("/" + "x" * 300000, "/p/password=hunter2xyz " + "x " * 150000, "/p/https://bob:pw@h " + "y " * 150000):
            self.fx.settings(darling=value)
            for name, arguments in (("launch_app", args), ("stop_prefix", {"prefix": "default", "confirm": True})):
                result = self.fx.call(name, arguments)
                self.assertTrue(result["isError"])
                text = result["content"][0]["text"]
                self.assertIn("not an absolute path to an executable file", text)
                self.assertLessEqual(len(text), mcp.Redactor.MAX_SCAN_CHARS + 200)
                self.assertNotIn("hunter", text)
                self.assertNotIn("bob:pw", text)

    def test_darling_is_found_on_path_when_settings_name_none(self):
        self.fx.settings()
        self.assertEqual(self.ok("launch_app", {"prefix": "default", "app": "Calc", "confirm": True})["exit_code"], 0)

    def test_stop_prefix_runs_shutdown_for_that_prefix(self):
        data = self.ok("stop_prefix", {"prefix": "default", "confirm": True})
        self.assertEqual(data["exit_code"], 0)
        self.assertIn("shutdown prefix=~/.darling-launcher/prefixes/default", data["output"])

    def test_stop_prefix_failure_is_an_error(self):
        self.fx.prefix("broken")
        self.refused("stop_prefix", {"prefix": "broken", "confirm": True}, "failed (exit 4): shutdown prefix=")

    def test_stop_prefix_timeout_leaves_the_command_running(self):
        self.fx.prefix("hang")
        with patch.object(mcp, "SHUTDOWN_TIMEOUT", 0.2):
            self.refused("stop_prefix", {"prefix": "hang", "confirm": True}, "not killed")
        self.assertEqual(len(self.fx.server.children), 1)
        self.fx.server.children[0].wait(timeout=10)
        self.assertTrue((self.fx.bin / "finished").exists())

    def volume(self):
        path = self.fx.data / "mounts/system"
        path.mkdir(parents=True, exist_ok=True)
        return str(path)

    def test_import_langid_data_runs_the_launcher_cli_for_that_prefix(self):
        self.fx.executable("darling-launcher", FAKE_LAUNCHER)
        data = self.ok("import_langid_data", {"prefix": "default", "volume": self.volume(), "confirm": True})
        self.assertEqual(data["exit_code"], 0)
        self.assertIn("argv: langid-import --volume ~/.darling-launcher/mounts/system --prefix ", data["output"])
        self.assertIn("/prefixes/default", data["output"])

    def test_import_langid_data_refuses_before_running_anything(self):
        self.fx.executable("darling-launcher", FAKE_LAUNCHER)
        self.refused("import_langid_data", {"prefix": "default", "volume": self.volume()}, "missing required argument 'confirm'")
        self.refused("import_langid_data", {"prefix": "default", "volume": self.volume(), "confirm": False}, "confirm must be true")
        self.refused("import_langid_data", {"prefix": "../../etc", "volume": self.volume(), "confirm": True}, "prefix")
        self.refused("import_langid_data", {"prefix": "default", "volume": "relative/dir", "confirm": True}, "volume must be an absolute path")
        self.refused("import_langid_data", {"prefix": "default", "volume": "/mnt/../etc", "confirm": True}, "'..'")
        for volume in ("/mnt/system", "/home", str(self.fx.root), str(self.fx.data / "mountsX")):
            self.refused("import_langid_data", {"prefix": "default", "volume": volume, "confirm": True}, "volume must be inside the launcher mount root")
        (self.fx.data / "mounts").mkdir(parents=True, exist_ok=True)
        (self.fx.data / "mounts/link").symlink_to(self.fx.root)
        self.refused("import_langid_data", {"prefix": "default", "volume": str(self.fx.data / "mounts/link"), "confirm": True}, "refusing symlink")
        self.refused("import_langid_data", {"prefix": "default", "volume": "/mnt/sys\ntem", "confirm": True}, "control characters")

    def test_import_langid_data_reports_a_refused_import_and_a_missing_launcher(self):
        self.refused("import_langid_data", {"prefix": "default", "volume": self.volume(), "confirm": True}, "no darling-launcher executable")
        self.fx.executable("darling-launcher", FAKE_LAUNCHER)
        self.fx.prefix("broken")
        self.refused("import_langid_data", {"prefix": "broken", "volume": self.volume(), "confirm": True}, "failed (exit 1): argv: langid-import")

    def test_action_tools_declare_themselves_as_not_read_only(self):
        tools = self.fx.server.tools
        for name in ("launch_app", "import_langid_data", "stop_prefix"):
            self.assertFalse(tools[name].annotations["readOnlyHint"])
            self.assertTrue(tools[name].description.startswith("NOT read-only."))
            self.assertIn("confirm=true", tools[name].description)
        self.assertTrue(tools["stop_prefix"].annotations["destructiveHint"])
        for name in ("launcher_status", "list_prefixes", "list_apps", "mount_status", "tail_log"):
            self.assertTrue(tools[name].annotations["readOnlyHint"])

    def test_server_never_uses_a_shell_and_only_kills_its_own_findmnt_helper(self):
        lines = [line.strip() for line in SCRIPT.read_text().splitlines() if not line.lstrip().startswith("#")]
        for forbidden in ("shell=True", "os.system", "os.popen", "os.kill", "killpg", ".terminate(", "os.setuid"):
            self.assertEqual([l for l in lines if forbidden in l], [], forbidden)
        self.assertEqual([l for l in lines if ".kill(" in l], ["proc.kill()"])


class FakeStat:
    """lstat as a root-owned system would report it. A symlink keeps the 0777 mode Linux gives every symlink."""

    def __init__(self, real, uid, writable):
        self.st_uid = uid
        if stat.S_ISLNK(real.st_mode):
            self.st_mode = real.st_mode
        else:
            self.st_mode = real.st_mode | 0o020 if writable else real.st_mode & ~0o022


class MountPlanBase(ToolTestCase):
    """The fixture bin directory plays /usr/bin; the ownership rule is exercised through a fake lstat."""

    def setUp(self):
        super().setUp()
        for name in ("pkexec", "sudo", "mount", "umount", "apfs-fuse", "apfsutil", "python3"):
            self.fx.executable(name, f'touch "$(dirname "$0")/ran-{name}"\n')
        self.mounts = self.fx.data / "mounts"
        self.not_root = set()
        self.writable = set()
        self.libexec = self.fx.root / "libexec/darling-launcher"
        self.libexec.mkdir(parents=True)
        for name in ("mount-macos.py", "mount-cryptex.py"):
            (self.libexec / name).write_text("raise SystemExit(0)\n")
        patcher = patch.multiple(mcp, TRUSTED_DIRECTORIES=(str(self.fx.bin),), INSTALLED_SCRIPT_DIRECTORIES=(str(self.libexec),), LSTAT=self.fake_lstat)
        patcher.start()
        self.addCleanup(patcher.stop)

    def fake_lstat(self, path):
        real = os.lstat(path)
        return FakeStat(real, 1000 if str(path) in self.not_root else 0, str(path) in self.writable)

    def plan(self, **arguments):
        return self.ok("mount_plan", arguments)

    def never_ran(self):
        self.assertEqual([p.name for p in self.fx.bin.glob("ran-*")], [])


class MountPlanTest(MountPlanBase):

    def test_batch_plan_mirrors_the_gui_command(self):
        data = self.plan(kind="batch")
        self.assertIs(data["executed"], False)
        (command,) = data["commands"]
        argv = command["argv"]
        self.assertTrue(argv[0].endswith("/bin/python3"))
        self.assertEqual(argv[1], "-I")
        self.assertTrue(argv[2].endswith("/libexec/darling-launcher/mount-macos.py"))
        # tools/mount-macos.py takes only --device, --volume and --kernel: no mount root and no tool paths, which it
        # finds itself in fixed root-owned locations. Any other option makes it exit 2.
        self.assertEqual(len(argv), 3)
        self.assertNotIn("--mount-root", command["shell"])
        self.assertNotIn("--apfs-fuse", command["shell"])
        self.assertIn("mount-macos.py", command["shell"])
        self.assertIn("/run/darling-launcher/", command["description"])
        self.never_ran()

    def test_batch_plan_takes_no_mount_root(self):
        self.refused("mount_plan", {"kind": "batch", "mount_root": str(self.mounts)}, "mount_root")

    def test_cryptex_plan_accepts_a_subdirectory_and_quotes_it(self):
        data = self.plan(kind="cryptex", mount_root=str(self.mounts / "my disks"))
        self.assertIn('"$HOME"\'/.darling-launcher/mounts/my disks\'', data["commands"][0]["shell"])
        self.assertTrue(any("does not exist" in w for w in data["warnings"]))

    def test_partition_plan_with_apfs_fuse(self):
        directory = self.mounts / "disk1"
        directory.mkdir()
        data = self.plan(kind="partition", device="/dev/nvme0n1p5", filesystem="apfs", backend="apfs-fuse", volume_index=1, directory=str(directory))
        argv = data["commands"][0]["argv"]
        self.assertTrue(argv[0].endswith("/bin/pkexec"))
        self.assertTrue(argv[1].endswith("/bin/apfs-fuse"))
        self.assertEqual(argv[2:5], ["-v", "1", "-o"])
        self.assertEqual(argv[5], f"ro,nodev,nosuid,noexec,uid={os.getuid()},gid={os.getgid()},allow_other")
        self.assertEqual(argv[6:], ["/dev/nvme0n1p5", "~/.darling-launcher/mounts/disk1"])
        self.assertEqual(data["commands"][0]["run_as"], "root via pkexec")
        self.assertEqual(data["warnings"], [])
        self.never_ran()

    def test_partition_plan_with_the_kernel_driver(self):
        apfs = self.plan(kind="partition", device="/dev/sda2", filesystem="apfs", backend="kernel", volume_index=0)["commands"][0]["argv"]
        self.assertTrue(apfs[1].endswith("/bin/mount"))
        self.assertEqual(apfs[2:5], ["-i", "-t", "apfs"])
        self.assertEqual(apfs[6], f"ro,nodev,nosuid,noexec,uid={os.getuid()},gid={os.getgid()},vol=0")
        self.assertEqual(apfs[7:], ["--", "/dev/sda2", "~/.darling-launcher/mounts/sda2-manual"])
        hfs = self.plan(kind="partition", device="/dev/sda3", filesystem="hfsplus", backend="kernel")["commands"][0]["argv"]
        self.assertNotIn("vol=", hfs[6])
        self.assertTrue(hfs[6].startswith("ro,nodev,nosuid,noexec"))

    def test_partition_plan_warns_about_the_directory(self):
        data = self.plan(kind="partition", device="/dev/sda3", filesystem="hfs", backend="kernel")
        self.assertTrue(any("does not exist yet" in w for w in data["warnings"]))
        (self.mounts / "sda3-manual").mkdir()
        (self.mounts / "sda3-manual/file").write_text("x")
        self.assertTrue(any("not empty" in w for w in self.plan(kind="partition", device="/dev/sda3", filesystem="hfs", backend="kernel")["warnings"]))

    def test_partition_argument_errors(self):
        base = {"kind": "partition", "device": "/dev/sda2", "filesystem": "apfs", "backend": "kernel", "volume_index": 0}
        cases = [({"device": "sda2"}, "under /dev"), ({"device": "/dev/../etc/passwd"}, "under /dev"), ({"device": "/dev/sda2;reboot"}, "under /dev"),
                 ({"device": "/dev/sda 2"}, "under /dev"), ({"volume_index": None}, "volume index"), ({"filesystem": "hfs", "backend": "apfs-fuse"}, "only supports APFS"),
                 ({"filesystem": "ext4"}, "must be one of"), ({"backend": "fuse"}, "must be one of"), ({"volume_index": 100}, "between 0 and 99"),
                 ({"device": None}, "needs device"), ({"filesystem": None}, "needs filesystem"), ({"backend": None}, "needs backend"),
                 ({"directory": "relative"}, "absolute path"), ({"directory": "/etc"}, "outside the launcher mount root"),
                 ({"directory": str(self.mounts)}, "not the root itself"), ({"directory": f"{self.mounts}/../x"}, "'.', '..'"),
                 ({"directory": f"{self.mounts}/a/./b"}, "'.', '..'"), ({"directory": str(self.fx.root / "elsewhere")}, "outside the launcher mount root"),
                 ({"directory": f"{self.mounts}x/disk"}, "outside the launcher mount root"), ({"mount_root": str(self.mounts)}, "not used by kind")]
        for change, fragment in cases:
            arguments = {k: v for k, v in {**base, **change}.items() if v is not None}
            self.refused("mount_plan", arguments, fragment)
        self.never_ran()

    def test_symlinked_mount_directories_are_refused(self):
        outside = self.fx.root / "outside"
        outside.mkdir()
        (self.mounts / "link").symlink_to(outside)
        self.refused("mount_plan", {"kind": "unmount", "directory": str(self.mounts / "link")}, "refusing symlink")
        self.refused("mount_plan", {"kind": "unmount", "directory": str(self.mounts / "link/inner")}, "refusing symlink")
        self.refused("mount_plan", {"kind": "cryptex", "mount_root": str(self.mounts / "link")}, "refusing symlink")
        self.refused("mount_plan", {"kind": "partition", "device": "/dev/sda1", "filesystem": "hfs", "backend": "kernel", "directory": str(self.mounts / "link/x")}, "refusing symlink")

    def test_mount_root_behind_a_symlink_is_accepted_by_either_spelling(self):
        real = self.fx.root / "real-mounts"
        real.mkdir()
        shutil.rmtree(self.mounts)
        self.mounts.symlink_to(real)
        for spelling in (str(self.mounts / "disk"), str(real / "disk")):
            argv = self.plan(kind="unmount", directory=spelling)["commands"][0]["argv"]
            self.assertTrue(argv[-1].endswith("/real-mounts/disk"))

    def test_unmount_plan(self):
        (self.mounts / "disk1").mkdir()
        data = self.plan(kind="unmount", directory=str(self.mounts / "disk1"))
        argv = data["commands"][0]["argv"]
        self.assertTrue(argv[0].endswith("/bin/pkexec") and argv[1].endswith("/bin/umount"))
        self.assertEqual(argv[2:], ["--", "~/.darling-launcher/mounts/disk1"])
        self.refused("mount_plan", {"kind": "unmount"}, "needs directory")
        self.refused("mount_plan", {"kind": "unmount", "directory": "/mnt/other"}, "outside the launcher mount root")
        self.refused("mount_plan", {"kind": "unmount", "directory": str(self.mounts)}, "not the root itself")
        self.never_ran()

    def test_cryptex_plan_mirrors_the_branch_command(self):
        data = self.plan(kind="cryptex", device="/dev/nvme0n1p5")
        argv = data["commands"][0]["argv"]
        self.assertEqual(argv[1], "-I")
        self.assertTrue(argv[2].endswith("/libexec/darling-launcher/mount-cryptex.py"))
        self.assertEqual(argv[3:5], ["--mount-root", "~/.darling-launcher/mounts"])
        self.assertTrue(argv[5] == "--apfs-fuse" and argv[6].endswith("/bin/apfs-fuse"))
        self.assertEqual(argv[7:], ["--device", "/dev/nvme0n1p5"])
        self.assertEqual(data["warnings"], [])
        self.assertNotIn("--device", self.plan(kind="cryptex")["commands"][0]["argv"])
        self.refused("mount_plan", {"kind": "cryptex", "device": "/dev/../x"}, "under /dev")
        self.refused("mount_plan", {"kind": "cryptex", "volume_index": 1}, "not used by kind")

    def test_scripts_missing_from_the_install_location_give_no_command_and_never_fall_back_to_the_checkout(self):
        checkout = Path(os.path.realpath(SCRIPT)).parent
        self.assertTrue((checkout / "mount-macos.py").is_file())
        for name in ("mount-macos.py", "mount-cryptex.py"):
            (self.libexec / name).unlink()
        for kind in ("batch", "cryptex"):
            data = self.plan(kind=kind)
            self.assertEqual(data["commands"], [])
            self.assertEqual(len(data["warnings"]), 1)
            self.assertIn("cmake --install", data["warnings"][0])
            self.assertIn("'partition' and 'unmount' need no script and work now", data["warnings"][0])
            self.assertIn(str(self.libexec).replace(str(self.fx.root), ""), data["warnings"][0])
            self.assertNotIn(str(checkout), json.dumps(data))
        description = self.fx.server.tools["mount_plan"].description
        self.assertNotIn("PR #24", description)
        self.assertIn("cmake --install", description)
        self.plan(kind="partition", device="/dev/sda2", filesystem="hfs", backend="kernel")["commands"][0]
        self.never_ran()

    def test_the_second_install_location_is_used_when_the_first_has_no_script(self):
        other = self.fx.root / "usr-libexec/darling-launcher"
        other.mkdir(parents=True)
        (other / "mount-macos.py").write_text("x\n")
        (self.libexec / "mount-macos.py").unlink()
        with patch.object(mcp, "INSTALLED_SCRIPT_DIRECTORIES", (str(self.libexec), str(other))):
            argv = self.plan(kind="batch")["commands"][0]["argv"]
        self.assertTrue(argv[2].endswith("/usr-libexec/darling-launcher/mount-macos.py"))

    def test_an_installed_script_must_be_root_owned_in_its_whole_chain(self):
        script = str(self.libexec / "mount-macos.py")
        for label, victim in (("file", script), ("directory", str(self.libexec)), ("parent", str(self.libexec.parent))):
            for attribute in ("not_root", "writable"):
                setattr(self, attribute, {victim})
                self.refused("mount_plan", {"kind": "batch"}, "must be owned by root and not group- or world-writable")
                setattr(self, attribute, set())
        self.plan(kind="batch")

    def test_a_symlinked_installed_script_is_judged_by_its_target(self):
        planted = self.fx.data / "prefixes/default/evil.py"
        planted.parent.mkdir(parents=True)
        planted.write_text("print('root')\n")
        (self.libexec / "mount-macos.py").unlink()
        (self.libexec / "mount-macos.py").symlink_to(planted)
        self.not_root = {str(planted)}
        self.refused("mount_plan", {"kind": "batch"}, "must be owned by root")
        self.never_ran()

    def test_an_installed_script_must_be_a_regular_file(self):
        (self.libexec / "mount-macos.py").unlink()
        (self.libexec / "mount-macos.py").mkdir()
        self.refused("mount_plan", {"kind": "batch"}, "is not a regular file")

    def test_missing_system_tools_fail_loudly(self):
        for tool, arguments in (("apfs-fuse", {"kind": "cryptex"}), ("python3", {"kind": "batch"}), ("pkexec", {"kind": "unmount", "directory": str(self.mounts / "d")}),
                                ("umount", {"kind": "unmount", "directory": str(self.mounts / "d")}), ("mount", {"kind": "partition", "device": "/dev/sda2", "filesystem": "hfs", "backend": "kernel"}),
                                ("apfs-fuse", {"kind": "partition", "device": "/dev/sda2", "filesystem": "apfs", "backend": "apfs-fuse", "volume_index": 0})):
            hidden = self.fx.bin / f"{tool}.hidden"
            (self.fx.bin / tool).rename(hidden)
            self.refused("mount_plan", arguments, f"{tool} was not found in ")
            hidden.rename(self.fx.bin / tool)

    def test_batch_plan_without_apfs_tools_warns_and_keeps_the_helper_command(self):
        for missing in (("apfs-fuse", "apfsutil"), ("apfs-fuse",), ("apfsutil",)):
            for tool in missing:
                (self.fx.bin / tool).rename(self.fx.bin / f"{tool}.hidden")
            data = self.plan(kind="batch")
            argv = data["commands"][0]["argv"]
            self.assertEqual(len(argv), 3)  # the helper takes no tool paths, so nothing is passed for a missing tool
            self.assertEqual(len(data["warnings"]), len(missing))
            self.assertTrue(all("HFS partitions are unaffected" in w for w in data["warnings"]))
            self.assertTrue(syntax_ok(data["commands"][0]["shell"]))
            for tool in missing:
                (self.fx.bin / f"{tool}.hidden").rename(self.fx.bin / tool)
        (self.fx.bin / "apfsutil").rename(self.fx.bin / "apfsutil.x")
        self.not_root = {str(self.fx.bin / "apfs-fuse")}
        self.refused("mount_plan", {"kind": "batch"}, "must be owned by root")
        self.not_root = set()

    def test_directories_on_path_are_never_trusted_even_if_the_fake_says_root_owns_them(self):
        other = self.fx.root / "other-bin"
        other.mkdir()
        (other / "pkexec").write_text("#!/bin/sh\n")
        (other / "pkexec").chmod(0o755)
        (self.fx.bin / "pkexec").unlink()
        self.not_root = {str(other)}
        with patch.dict(os.environ, {"PATH": f"{other}:{self.fx.bin}:/usr/bin:/bin"}):
            self.refused("mount_plan", {"kind": "unmount", "directory": str(self.mounts / "d")}, "pkexec was not found in ")

    def test_caller_supplied_tool_paths_are_rejected_as_unknown_arguments(self):
        evil = self.fx.data / "prefixes/default/evil"
        evil.parent.mkdir(parents=True)
        evil.write_text("#!/bin/sh\n")
        evil.chmod(0o755)
        for name in ("apfs_fuse", "apfsutil", "pkexec", "python"):
            for kind in ("batch", "cryptex", "partition", "unmount"):
                self.refused("mount_plan", {"kind": kind, name: str(evil)}, "unknown argument")
        self.never_ran()

    def test_system_tools_must_be_root_owned_and_not_writable_in_the_whole_chain(self):
        arguments = {"kind": "unmount", "directory": str(self.mounts / "d")}
        tool = os.path.realpath(self.fx.bin / "pkexec")
        for label, victim in (("the file", tool), ("the directory", str(self.fx.bin)), ("an ancestor", str(self.fx.root))):
            self.not_root = {victim}
            self.refused("mount_plan", arguments, "must be owned by root and not group- or world-writable")
            self.not_root = set()
            self.writable = {victim}
            self.refused("mount_plan", arguments, "must be owned by root and not group- or world-writable")
            self.writable = set()
        self.plan(**arguments)

    def test_a_root_owned_symlink_to_a_root_owned_binary_is_accepted_despite_its_0777_mode(self):
        real = self.fx.bin / "pkexec.real"
        (self.fx.bin / "pkexec").rename(real)
        (self.fx.bin / "pkexec").symlink_to(real.name)
        self.assertEqual(stat.S_IMODE(os.lstat(self.fx.bin / "pkexec").st_mode), 0o777)
        self.assertEqual(stat.S_IMODE(self.fake_lstat(self.fx.bin / "pkexec").st_mode), 0o777)
        argv = self.plan(kind="unmount", directory=str(self.mounts / "d"))["commands"][0]["argv"]
        self.assertTrue(argv[0].endswith("/bin/pkexec.real"))

    def test_a_symlinked_directory_in_the_chain_is_accepted_when_root_owned(self):
        real_dir = self.fx.root / "usr-bin"
        self.fx.bin.rename(real_dir)
        self.fx.bin.symlink_to(real_dir)
        self.assertTrue(self.plan(kind="unmount", directory=str(self.mounts / "d"))["commands"][0]["argv"][0].endswith("/usr-bin/pkexec"))

    def test_a_user_owned_symlink_is_refused_even_if_its_target_is_fine(self):
        real = self.fx.bin / "pkexec.real"
        (self.fx.bin / "pkexec").rename(real)
        (self.fx.bin / "pkexec").symlink_to(real.name)
        self.not_root = {str(self.fx.bin / "pkexec")}
        self.refused("mount_plan", {"kind": "unmount", "directory": str(self.mounts / "d")}, "must be owned by root")

    def test_a_symlink_to_a_user_writable_or_user_owned_target_is_refused(self):
        real = self.fx.bin / "pkexec.real"
        (self.fx.bin / "pkexec").rename(real)
        (self.fx.bin / "pkexec").symlink_to(real.name)
        for victims in ("writable", "not_root"):
            setattr(self, victims, {str(real)})
            self.refused("mount_plan", {"kind": "unmount", "directory": str(self.mounts / "d")}, "must be owned by root and not group- or world-writable")
            setattr(self, victims, set())
        self.plan(kind="unmount", directory=str(self.mounts / "d"))

    def test_a_symlink_in_a_system_directory_is_judged_by_its_target(self):
        guest = self.fx.data / "prefixes/default/evil"
        guest.parent.mkdir(parents=True)
        guest.write_text("#!/bin/sh\n")
        guest.chmod(0o755)
        (self.fx.bin / "pkexec").unlink()
        (self.fx.bin / "pkexec").symlink_to(guest)
        self.not_root = {str(guest)}
        self.refused("mount_plan", {"kind": "unmount", "directory": str(self.mounts / "d")}, "must be owned by root")

    def test_system_tool_must_be_an_executable_file(self):
        (self.fx.bin / "pkexec").chmod(0o644)
        self.refused("mount_plan", {"kind": "unmount", "directory": str(self.mounts / "d")}, "is not an executable file")

    def test_real_system_directories_pass_the_ownership_rule(self):
        with patch.multiple(mcp, TRUSTED_DIRECTORIES=("/usr/local/bin", "/usr/bin"), LSTAT=os.lstat):
            if not os.path.lexists("/usr/bin/sh"):
                self.skipTest("no /usr/bin/sh")
            self.assertTrue(os.path.isfile(mcp.system_tool("sh")))
            with self.assertRaises(mcp.ToolError):
                mcp.system_tool("definitely-not-installed-xyz")

    def test_no_tool_ever_runs_pkexec_or_sudo(self):
        fx = self.fx
        fx.fake_findmnt([])
        fx.prefix("default", [("Calc", "Calc")])
        for name, arguments in (("launcher_status", {}), ("list_prefixes", {}), ("list_apps", {"prefix": "default"}), ("mount_status", {}), ("tail_log", {}),
                                ("launch_app", {"prefix": "default", "app": "Calc", "confirm": True}), ("stop_prefix", {"prefix": "default", "confirm": True})):
            fx.call(name, arguments)
        for kind in ("batch", "cryptex"):
            fx.call("mount_plan", {"kind": kind})
        self.never_ran()


SH = shutil.which("sh") or "/bin/sh"


def shell_words(shell, home, cwd):
    """What a POSIX shell with HOME set makes of the string: the words printf receives, NUL separated, and nothing else run."""
    done = subprocess.run([SH, "-c", "printf '%s\\0' " + shell], capture_output=True, env={"HOME": str(home), "PATH": "/usr/bin:/bin"}, cwd=cwd, timeout=10)
    assert done.returncode == 0, done.stderr
    return done.stdout.decode().split("\0")[:-1]


def syntax_ok(shell):
    return subprocess.run([SH, "-n", "-c", shell], capture_output=True, timeout=10).returncode == 0


def hide_user(arg):
    return re.sub(r"(?i)/(home|Users|run/media)/[^/]+", lambda m: f"/{m.group(1)}/<user>", arg)


class MountPlanShellTest(MountPlanBase):
    """argv and descriptions are redacted for the model; the shell string hides the home path and user name yet still runs."""

    fixture_args = {"home_name": "My Mac"}

    def raw(self, **arguments):
        return self.fx.server.tools["mount_plan"].handler(arguments)[0]

    def directory(self, name):
        directory = self.fx.data / "mounts" / name
        directory.mkdir(parents=True)
        return directory

    def test_argv_description_and_warnings_are_redacted(self):
        directory = self.directory("disk 1")
        (directory / "f").write_text("x")
        result = self.fx.call("mount_plan", {"kind": "partition", "device": "/dev/sda1", "filesystem": "hfs", "backend": "kernel", "directory": str(directory)})
        data = result["structuredContent"]
        command = data["commands"][0]
        self.assertEqual(command["argv"][-1], "~/.darling-launcher/mounts/disk 1")
        self.assertIn("~/.darling-launcher/mounts/disk 1", command["description"])
        self.assertIn("~/.darling-launcher/mounts/disk 1 is not empty", data["warnings"][0])
        for place in (command["argv"], command["description"], data["warnings"], data["notes"]):
            self.assertNotIn("My Mac", json.dumps(place))
        self.assertNotIn(str(self.fx.home), json.dumps(result))
        self.assertNotIn("/home/cristi", json.dumps(result))

    def test_shell_string_uses_dollar_home_and_names_neither_home_nor_user(self):
        directory = self.directory("disk 1")
        result = self.fx.call("mount_plan", {"kind": "unmount", "directory": str(directory)})
        shell = result["structuredContent"]["commands"][0]["shell"]
        self.assertTrue(shell.endswith("""-- "$HOME"'/.darling-launcher/mounts/disk 1'"""), shell)
        for secret in ("My Mac", str(self.fx.home), "/home/cristi"):
            self.assertNotIn(secret, json.dumps(result))
        self.assertTrue(any('"$HOME"' in note for note in result["structuredContent"]["notes"]))

    def test_shell_string_runs_and_resolves_to_the_real_argv(self):
        names = ("plain", "with space", "a;b", "x$HOME", "back`id`", "q'uote", "$(reboot)", "~tilde", "-dash", "a&b|c>d", "sp ace  s", "*glob?", "a\\b", "!bang", "é ü")
        with tempfile.TemporaryDirectory() as cwd:
            for name in names:
                directory = self.directory(name)
                raw = self.raw(kind="unmount", directory=str(directory))["commands"][0]
                self.assertEqual(shell_words(raw["shell"], self.fx.home, cwd)[-1], str(directory), name)
                self.assertEqual(os.listdir(cwd), [], name)
            for arguments in ({"kind": "batch"},
                              {"kind": "cryptex", "mount_root": str(self.fx.data / "mounts/p q"), "device": "/dev/sda1"},
                              {"kind": "partition", "device": "/dev/sda1", "filesystem": "apfs", "backend": "kernel", "volume_index": 2, "directory": str(self.fx.data / "mounts/p q/d")}):
                command = self.raw(**arguments)["commands"][0]
                self.assertTrue(syntax_ok(command["shell"]))
                self.assertEqual(shell_words(command["shell"], self.fx.home, cwd), [hide_user(a) if not a.startswith(str(self.fx.home)) else a for a in command["argv"]])

    ADVERSARIAL = ("a b", "q'uote", '"dq"', "$(touch pwned)", "`touch pwned`", "x;touch pwned", "a&&touch pwned", "a|b", "a>b<c", "%27", "100%", "a%27b%3Btouch%20pwned",
                   "%2Fhome%2Falice", "password=x y", "token='a b'", "tok='; touch pwned; '", "Bearer abc def", 'x password="a b" y', "password='x", "pass'word=abc def",
                   "token=a'b c", "x token ' y; touch z; ' abcdefgh", "ghp_" + "A1b2C3d4E5" * 3, "sk-" + "a" * 24, "password hunter2xyz", "-rf", "~", "~root", "a\\b",
                   "'", '"', "''", "'\"'\"'", "*", "{a,b}", "a#b", "!x", "a=b", "$HOME", "${HOME}", "$'x'")

    def test_the_shell_string_always_expands_to_exactly_the_final_argv(self):
        redact = self.fx.server.redact
        with tempfile.TemporaryDirectory() as cwd:
            for name in self.ADVERSARIAL:
                directory = self.directory(name)
                raw = self.raw(kind="unmount", directory=str(directory))["commands"][0]
                shell = raw["shell"]
                final = [hide_user(redact.text(a, paths=False)) if not a.startswith(str(self.fx.home)) else redact.text(a, paths=False) for a in raw["argv"]]
                self.assertTrue(syntax_ok(shell), name)
                self.assertEqual(shell_words(shell, self.fx.home, cwd), final, name)
                self.assertEqual(os.listdir(cwd), [], name)
                self.assertEqual(shlex.split(shell.replace('"$HOME"', "'" + str(self.fx.home).replace("'", "'\"'\"'") + "'")), final, name)
                self.assertEqual(self.fx.call("mount_plan", {"kind": "unmount", "directory": str(directory)})["structuredContent"]["commands"][0]["shell"], shell,
                                 f"{name}: nothing may rewrite the shell string after it was quoted")

    def test_secret_like_text_in_a_path_is_removed_before_quoting_not_after(self):
        for name, absent in (("ghp_" + "A1b2C3d4E5" * 3, "ghp_"), ("password=hunter2hunter2", "hunter2"), ("Bearer abcdef123", "abcdef123"), ("password hunter2xyz", "hunter2")):
            command = self.raw(kind="unmount", directory=str(self.directory(name)))["commands"][0]
            self.assertNotIn(absent, command["shell"])
            self.assertIn("REDACTED", command["shell"])
            self.assertNotIn("<redacted>", command["shell"])
        result = self.fx.call("mount_plan", {"kind": "unmount", "directory": str(self.directory("password=zzzzzz"))})["structuredContent"]
        self.assertTrue(any("REDACTED" in note and "will not work" in note for note in result["notes"]))

    def test_the_builder_refuses_a_plan_whose_shell_string_would_not_match_the_argv(self):
        directory = self.directory("d")
        with patch.object(mcp, "shell_word", lambda arg, homes: arg):
            spaced = self.directory("has space")
            self.refused("mount_plan", {"kind": "unmount", "directory": str(spaced)}, "cannot build a safe shell string")
        with patch.object(mcp, "shlex") as fake:
            fake.quote = lambda arg: "'" + arg + "'"
            fake.split = mcp.shlex.split
            self.refused("mount_plan", {"kind": "unmount", "directory": str(self.directory("q'uote"))}, "cannot build a safe shell string")
        self.ok("mount_plan", {"kind": "unmount", "directory": str(directory)})

    def test_newlines_and_control_characters_in_names_are_refused(self):
        directory = self.fx.data / "mounts" / "bad\nname"
        directory.mkdir(parents=True)
        self.refused("mount_plan", {"kind": "unmount", "directory": str(directory)}, "control characters")
        self.refused("mount_plan", {"kind": "unmount", "directory": str(self.fx.data / "mounts/a\tb")}, "control characters")

    def test_every_plan_kind_has_a_syntactically_valid_shell_string(self):
        for arguments in ({"kind": "batch"}, {"kind": "cryptex"}, {"kind": "unmount", "directory": str(self.directory("d"))},
                          {"kind": "partition", "device": "/dev/sda1", "filesystem": "hfs", "backend": "kernel"}):
            for command in self.ok("mount_plan", arguments)["commands"]:
                self.assertTrue(syntax_ok(command["shell"]), command["shell"])

    def test_secrets_are_still_removed_from_the_shell_string(self):
        directory = self.directory("password=hunter2hunter2")
        shell = self.ok("mount_plan", {"kind": "unmount", "directory": str(directory)})["commands"][0]["shell"]
        self.assertNotIn("hunter2", shell)
        self.assertIn("password=REDACTED", shell)
        self.assertNotIn("<redacted>", shell)
        self.assertTrue(syntax_ok(shell))

    def test_the_shell_field_is_the_only_exemption(self):
        self.assertEqual(self.fx.server.tools["mount_plan"].shell_fields, frozenset({"shell"}))
        for name, tool in self.fx.server.tools.items():
            if name != "mount_plan":
                self.assertEqual(tool.shell_fields, frozenset(), name)


class MountPlanOutsideHomeTest(MountPlanBase):
    """A path outside HOME that carries a /home/<name> component gets the quoted placeholder, which cannot run anything unintended."""

    fixture_args = {"data_dir": "home/bob/launcher"}

    def test_the_user_name_becomes_a_quoted_placeholder_and_a_note_says_to_replace_it(self):
        directory = self.fx.data / "mounts/d"
        directory.mkdir(parents=True)
        result = self.ok("mount_plan", {"kind": "unmount", "directory": str(directory)})
        command = result["commands"][0]
        self.assertIn("/home/'<user>'/launcher/mounts/d", command["shell"])
        self.assertNotIn("bob", json.dumps(result))
        self.assertNotIn("/home/cristi", json.dumps(result))
        self.assertTrue(syntax_ok(command["shell"]))
        self.assertTrue(any("placeholder '<user>'" in note and "replace it" in note for note in result["notes"]))
        with tempfile.TemporaryDirectory() as cwd:
            words = shell_words(command["shell"], self.fx.home, cwd)
            self.assertEqual(words[-1], hide_user(str(directory)))
            self.assertEqual(os.listdir(cwd), [])


class ShellWordTest(unittest.TestCase):
    def test_nasty_arguments_expand_to_themselves_including_newlines_and_percent_sequences(self):
        homes = mcp.home_expansions("/home/alice")
        with tempfile.TemporaryDirectory() as cwd:
            for arg in ("/x/a\nb", "/x/$(id)\n`id`;&&|", "/x/%27%22%0A", "/x/'", "/x/'\"'\"'", "/home/alice/a\nb c", "/home/alice/%27;$(x)", "/home/alice/'", "/Users/bob/\n$x", "-n", "", "/x/\\", "/x/\t"):
                word = mcp.shell_word(arg, homes)
                mcp.check_shell_word(arg, word, homes)
                expected = arg if arg.startswith("/home/alice") else hide_user(arg)
                self.assertTrue(syntax_ok("cmd " + word), repr(arg))
                self.assertEqual(shell_words(word, "/home/alice", cwd), [expected], repr(arg))
                self.assertEqual(mcp.word_value(word, homes), expected)
            self.assertEqual(os.listdir(cwd), [])


    def test_a_symlinked_home_is_expanded_through_realpath(self):
        with tempfile.TemporaryDirectory() as root, tempfile.TemporaryDirectory() as cwd:
            real = Path(os.path.realpath(root)) / "real home"
            real.mkdir()
            link = Path(os.path.realpath(root)) / "link"
            link.symlink_to(real)
            homes = mcp.home_expansions(link)
            self.assertEqual({e for e, _ in homes}, {'"$HOME"', '"$(realpath -- "$HOME")"'})
            for arg in (str(link / "x y"), str(real / "x y"), str(real), str(link)):
                words = shell_words(mcp.shell_word(arg, homes), link, cwd)
                self.assertEqual(words, [arg])

    def test_paths_outside_home_and_without_a_user_component_are_plainly_quoted(self):
        homes = mcp.home_expansions("/home/alice")
        self.assertEqual(mcp.shell_word("/usr/bin/umount", homes), "/usr/bin/umount")
        self.assertEqual(mcp.shell_word("/dev/sda 1", homes), "'/dev/sda 1'")
        self.assertEqual(mcp.shell_word("/home/alice2/x", homes), "/home/'<user>'/x")
        self.assertEqual(mcp.shell_word("/home/alice", homes), '"$HOME"')
        with tempfile.TemporaryDirectory() as cwd:
            word = mcp.shell_word("/Users/bob/a b/Users/c", homes)
            self.assertEqual(shell_words(word, "/home/alice", cwd), ["/Users/<user>/a b/Users/<user>"])


def environment_accesses(source):
    """Every way the source touches os.environ, from the AST: (names read by a bare string constant, accesses whose key is anything
    else). The whole key expression is checked, so "HOME" + suffix, an f-string, a format call, a conditional, a subscript or a
    name bound elsewhere are all computed accesses. Passing the whole environment is only allowed to Config.from_environ, and the
    SAFE_ENV_NAMES comprehension may index by its own loop variable (its constants are read from the tuple)."""
    tree = ast.parse(source)
    parents = {child: parent for parent in ast.walk(tree) for child in ast.iter_child_nodes(parent)}
    def is_environ(n):
        return (isinstance(n, ast.Name) and n.id == "environ") or (isinstance(n, ast.Attribute) and n.attr == "environ")
    names, computed = set(), []

    def key(node, where):
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            names.add(node.value)
            return
        for ancestor in (parents.get(node), parents.get(parents.get(node))):
            if isinstance(ancestor, ast.comprehension) or isinstance(ancestor, (ast.DictComp, ast.ListComp, ast.SetComp, ast.GeneratorExp)):
                for generator in ancestor.generators if hasattr(ancestor, "generators") else [ancestor]:
                    if isinstance(generator.iter, ast.Name) and generator.iter.id == "SAFE_ENV_NAMES" and isinstance(node, ast.Name) and node.id == generator.target.id:
                        return
        computed.append((where.lineno, ast.get_source_segment(source, where)))

    for node in ast.walk(tree):
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "SAFE_ENV_NAMES" for t in node.targets):
            names.update(e.value for e in node.value.elts)
    for node in ast.walk(tree):
        if not is_environ(node):
            continue
        parent = parents.get(node)
        if isinstance(parent, ast.Subscript) and parent.value is node:
            key(parent.slice, parent)
        elif isinstance(parent, ast.Attribute) and parent.value is node and isinstance(parents.get(parent), ast.Call) and parents[parent].func is parent:
            call = parents[parent]
            if parent.attr in ("get", "pop", "setdefault"):
                argument = call.args[0] if call.args else next((k.value for k in call.keywords if k.arg == "key"), None)
                key(argument, call) if argument is not None else computed.append((call.lineno, ast.get_source_segment(source, call)))
            else:
                computed.append((call.lineno, ast.get_source_segment(source, call)))
        elif isinstance(parent, ast.Compare) and node in parent.comparators and all(isinstance(op, (ast.In, ast.NotIn)) for op in parent.ops):
            key(parent.left, parent)
        elif isinstance(parent, ast.Call) and node in parent.args and getattr(parent.func, "attr", getattr(parent.func, "id", "")) == "from_environ":
            continue
        elif isinstance(parent, ast.arg) or isinstance(parent, ast.FunctionDef) or (isinstance(parent, ast.Attribute) and parent.attr == "environ"):
            continue
        else:
            computed.append((getattr(parent, "lineno", getattr(node, "lineno", 0)), ast.get_source_segment(source, parent) if parent is not None else ""))
    for node in ast.walk(tree):  # os.getenv(...) and getenv(...)
        if isinstance(node, ast.Call) and getattr(node.func, "attr", getattr(node.func, "id", "")) in ("getenv", "getenvb", "putenv", "unsetenv"):
            key(node.args[0], node) if node.args else computed.append((node.lineno, ast.get_source_segment(source, node)))
    return names, computed


class EnvironmentDocumentationTest(unittest.TestCase):
    def documented_names(self):
        readme = (SCRIPT.parent.parent / "README.md").read_text()
        section = readme.split("### Environment variables", 1)[1].split("\n### ", 1)[0].split("\n## ", 1)[0]
        names = set()
        for row in section.splitlines():
            if row.startswith("| `"):
                names.update(re.findall(r"`([A-Z][A-Z0-9_]*)`", row.split("|")[1]))
        return names

    def test_the_environment_variables_the_code_uses_are_the_documented_ones(self):
        used, computed = environment_accesses(SCRIPT.read_text())
        self.assertEqual(computed, [], "an environment variable is read by a computed key; read it by a bare string constant and document it")
        documented = self.documented_names()
        self.assertEqual(used - documented, set(), "the code uses environment variables the README does not document")
        self.assertEqual(documented - used, set(), "the README documents environment variables the code does not use")
        self.assertGreaterEqual(len(used), 12)

    def test_the_scan_checks_the_whole_key_expression(self):
        flagged = ['os.environ.get("HOME" + suffix)', 'os.environ.get(f"HOME{x}")', 'os.getenv("A" if x else "B")', 'os.environ["HO" + "ME" + x]', 'os.environ.get("HOME%s" % x)',
                   'os.environ.get("HOME{}".format(x))', 'os.environ.get("".join(parts))', 'os.environ[NAMES[0]]', 'os.environ.get(key)', 'os.environ.pop("A" + b)',
                   '("HOME" + x) in os.environ', 'os.environ.setdefault(prefix + "X", "1")', 'os.environ.get(key="A" + b)', 'os.getenv(name)', 'dict(os.environ)',
                   'for k in os.environ: pass', 'os.environ.copy()', 'os.environ.keys()', 'environ.get("A" + b)', 'os.environ.get()']
        for source in flagged:
            names, computed = environment_accesses("import os\n" + source + "\n")
            self.assertEqual(len(computed), 1, source)
        compliant = {'os.environ.get("HOME")': {"HOME"}, 'os.environ["HO" "ME"]': {"HOME"}, '"A" in os.environ': {"A"}, 'os.getenv("LANG", "C")': {"LANG"},
                     'os.environ.pop("X", None)': {"X"}, 'environ.get("PATH", "/bin")': {"PATH"}, 'os.environ.get(key="Q")': {"Q"},
                     "Config.from_environ(os.environ)": set(), '"B" not in os.environ': {"B"}}
        for source, expected in compliant.items():
            names, computed = environment_accesses("import os\n" + source + "\n")
            self.assertEqual((names, computed), (expected, []), source)

    def test_the_scan_reports_unlisted_names_and_the_safe_env_tuple(self):
        names, computed = environment_accesses('SAFE_ENV_NAMES = ("A", "B")\nx = {n: os.environ[n] for n in SAFE_ENV_NAMES if n in os.environ}\ny = os.environ.get("SOMETHING_NEW")\n')
        self.assertEqual((names, computed), ({"A", "B", "SOMETHING_NEW"}, []))
        names, computed = environment_accesses('SAFE_ENV_NAMES = ("A",)\nx = {n: os.environ[n] for n in OTHER_NAMES}\n')
        self.assertEqual(len(computed), 1)
        self.assertIn("SOMETHING_NEW", environment_accesses('y = os.environ.get("SOMETHING_NEW")')[0] - self.documented_names())


if __name__ == "__main__":
    unittest.main()
