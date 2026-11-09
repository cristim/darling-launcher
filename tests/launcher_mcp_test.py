#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for tools/launcher-mcp.py. Fixture based: temp dirs and fake executables only, no Darling, pkexec or network."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
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


class Fixture:
    """A fake home with a launcher data dir, settings file and fake executables on PATH."""

    def __init__(self, allow_actions=False):
        self.root = Path(os.path.realpath(tempfile.mkdtemp(prefix="launcher-mcp-test-")))
        self.home = self.root / "home" / USER
        self.data = self.home / ".darling-launcher"
        self.config_file = self.home / ".config/cristim/darling-launcher.conf"
        self.bin = self.root / "bin"
        for directory in (self.data / "prefixes", self.data / "logs", self.data / "mounts", self.data / "runtimes", self.config_file.parent, self.bin):
            directory.mkdir(parents=True)
        self.config = mcp.Config(self.home, self.data, self.config_file, allow_actions, USER)
        self.server = mcp.Server(self.config)
        self.env = patch.dict(os.environ, {"PATH": f"{self.bin}:/usr/bin:/bin", "HOME": str(self.home)})
        self.env.start()
        self.server.initialized = True

    def close(self):
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
    def setUp(self):
        self.fx = Fixture()
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
        self.proc = subprocess.Popen([sys.executable, "-I", str(SCRIPT)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment)

    def send_raw(self, data):
        self.proc.stdin.write(data)
        self.proc.stdin.flush()

    def send(self, message):
        self.send_raw(json.dumps(message).encode() + b"\n")

    def read(self):
        line = self.proc.stdout.readline()
        return json.loads(line) if line else None

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
        self.proc.wait(timeout=10)
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
        self.assertEqual({t["name"] for t in tools}, set(mcp.Server(mcp.Config("/h", "/h/d", "/h/c", False, USER)).tools))
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
            self.assertEqual(session.proc.wait(timeout=10), 2)
            self.assertIn("configuration error", session.close())

    def test_stdout_carries_only_protocol_and_logs_go_to_stderr(self):
        session = self.session()
        session.handshake()
        session.rpc("tools/call", {"name": "launcher_status", "arguments": {}}, request_id=2)
        session.proc.stdin.close()
        rest = session.proc.stdout.read()
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
               {"name": "E", "bundle": "Other/E.app", "executable": "e"}, "text", {"name": 3, "bundle": "Applications/F.app", "executable": "f"}]
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

    def test_mount_status_times_out(self):
        self.fx.executable("findmnt", "sleep 30\n")
        started = time.monotonic()
        with patch.object(mcp, "FINDMNT_TIMEOUT", 0.3):
            self.refused("mount_status", {}, "did not finish")
        self.assertLess(time.monotonic() - started, 5)

    def test_tail_log_limits_and_sources(self):
        fx = self.fx
        (fx.data / "logs/launcher.log").write_text("".join(f"line {i}\n" for i in range(300)) + "x" * 5000 + "\n")
        (fx.data / "logs/mcp-launch.log").write_text("launch output\n")
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

    def test_tail_log_missing_and_symlinked_logs(self):
        self.assertEqual(self.ok("tail_log")["lines"], [])
        secret = self.fx.root / "secret.txt"
        secret.write_text("top secret\n")
        (self.fx.data / "logs/launcher.log").symlink_to(secret)
        self.refused("tail_log", {}, "cannot open log")
        shutil.rmtree(self.fx.data / "logs")
        (self.fx.data / "logs").symlink_to(self.fx.root)
        self.refused("tail_log", {}, "refusing symlink")


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
        self.assertIn("opened ~/Documents/file by <user>", text)
        self.assertIn("/home/<user>/stuff", text)
        self.assertIn("alicesmith stays", text)
        status = json.dumps(fx.call("launcher_status"))
        self.assertNotIn(str(fx.home), status)
        self.assertNotIn(f"/{USER}/", status)
        self.assertIn("~/bin/darling", status)

    def test_error_messages_are_redacted_too(self):
        result = self.fx.call("list_apps", {"prefix": f"{self.fx.home}/private"})
        self.assertNotIn(str(self.fx.home), json.dumps(result))

    def test_redactor_handles_root_and_short_user_names(self):
        redactor = mcp.Redactor("/root", "root")
        self.assertEqual(redactor.text("/root/x and root"), "~/x and root")
        self.assertEqual(mcp.Redactor("/home/a", "a").text("a /home/a"), "a ~")


if __name__ == "__main__":
    unittest.main()
