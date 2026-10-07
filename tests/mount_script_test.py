#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("mount_macos", Path(__file__).resolve().parents[1] / "tools/mount-macos.py")
mount = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mount)

class MountScriptTest(unittest.TestCase):
    def test_one_authorization_and_no_retry(self):
        for code in (0, 126, 127):
            with patch.object(mount.shutil, "which", return_value="/usr/bin/pkexec"), patch.object(mount.subprocess, "call", return_value=code) as call, patch.object(mount, "emit"):
                self.assertEqual(mount.main(["--mount-root", "/tmp/path with spaces", "--apfs-fuse", "/tmp/fuse", "--apfsutil", "/tmp/util"]), code)
                self.assertEqual(call.call_count, 1)
                command = call.call_args.args[0]
                self.assertEqual(command[0], "/usr/bin/pkexec")
                self.assertIn("--privileged", command)
                self.assertIn("/tmp/path with spaces", command)

    def test_metadata_encryption_and_invalid_indices(self):
        result = mount.volumes("Volume 0 12345678-1234-1234-1234-123456789ABC\nName: macOS\nFileVault: No\nVolume 1 12345678-1234-1234-1234-123456789ABC\nFileVault: Yes\nVolume 100 12345678-1234-1234-1234-123456789ABC\nFileVault: No\n")
        self.assertEqual(len(result), 2)
        self.assertFalse(result[0]["encrypted"])
        self.assertTrue(result[1]["encrypted"])

    def test_readonly_batch_and_source_discovery(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            util, fuse = root / "util", root / "fuse"
            for helper in (util, fuse):
                helper.write_text("#!/bin/sh\nexit 0\n")
                helper.chmod(0o700)
            options = SimpleNamespace(mount_root=str(root / "mounts"), apfsutil=str(util), apfs_fuse=str(fuse))
            calls, records, events = [], [], []
            def run(arguments):
                calls.append(arguments)
                if arguments[0] == "/usr/bin/lsblk":
                    return json.dumps({"blockdevices": [{"path": "/dev/synthetic1", "fstype": "apfs", "mountpoints": []}, {"path": "/dev/already", "fstype": "apfs", "mountpoints": ["/existing"]}]})
                if arguments[0] == "/usr/bin/findmnt":
                    return json.dumps({"filesystems": records})
                if arguments[0] == str(util):
                    return "Volume 0 12345678-1234-1234-1234-123456789ABC\nName: macOS\nFileVault: No\nVolume 1 12345678-1234-1234-1234-123456789ABC\nFileVault: Yes\n"
                self.assertEqual(arguments[0], str(fuse))
                self.assertIn("ro,nodev,nosuid,noexec,uid=1000,gid=1000,allow_other", arguments)
                destination = Path(arguments[-1])
                (destination / "System/Library").mkdir(parents=True)
                (destination / "System/Applications").mkdir()
                records.append({"source": arguments[-2], "target": str(destination), "options": "ro,nodev,nosuid,noexec"})
                return ""
            owned = mount.mount_batch(options, 1000, 1000, run, lambda kind, **fields: events.append({"event": kind, **fields}), lambda _: None)
            self.assertEqual(len(owned), 1)
            self.assertEqual(events[-1]["sources"], owned)
            self.assertEqual(sum(c[0] == str(fuse) for c in calls), 1)
            self.assertTrue(any(e.get("reason", "").startswith("Encrypted") for e in events))
            self.assertTrue(any(e.get("reason") == "Already mounted" for e in events))

    def test_writable_mount_is_not_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("util", "fuse"):
                (root / name).write_text("fixture")
                (root / name).chmod(0o700)
            options = SimpleNamespace(mount_root=str(root / "mounts"), apfsutil=str(root / "util"), apfs_fuse=str(root / "fuse"))
            records, events = [], []
            def run(arguments):
                if arguments[0] == "/usr/bin/lsblk": return json.dumps({"blockdevices": [{"path": "/dev/synthetic", "fstype": "apfs"}]})
                if arguments[0] == "/usr/bin/findmnt": return json.dumps({"filesystems": records})
                if arguments[0] == str(root / "util"): return "Volume 0 12345678-1234-1234-1234-123456789ABC\nFileVault: No\n"
                if arguments[0] == "/usr/bin/umount":
                    records.clear()
                    return ""
                records.append({"source": arguments[-2], "target": arguments[-1], "options": "rw,nodev,nosuid,noexec"})
                return ""
            self.assertEqual(mount.mount_batch(options, 1000, 1000, run, lambda kind, **fields: events.append({"event": kind, **fields}), lambda _: None), [])
            self.assertTrue(any(e["event"] == "error" and "verify" in e["message"] for e in events))

    def test_symlink_root_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "link").symlink_to(root, target_is_directory=True)
            options = SimpleNamespace(mount_root=str(root / "link"))
            with self.assertRaises(ValueError):
                mount.mount_batch(options, 1000, 1000)

    def test_privileged_mode_requires_pkexec_identity(self):
        with patch.object(mount.os, "geteuid", return_value=1000), patch.object(mount, "emit"):
            self.assertEqual(mount.main(["--privileged", "--mount-root", "/tmp/test"]), 1)

if __name__ == "__main__":
    unittest.main()
