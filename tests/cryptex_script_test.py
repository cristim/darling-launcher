#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("mount_cryptex", Path(__file__).resolve().parents[1] / "tools/mount-cryptex.py")
cryptex = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cryptex)
base = cryptex.base

UUID_A = "12345678-1234-1234-1234-123456789ABC"
UUID_B = "87654321-4321-4321-4321-CBA987654321"
LIST = ("Volume 0 12345678-1234-1234-1234-123456789ABC\nName: Macintosh HD\nFileVault: No\n"
        "Volume 3 12345678-1234-1234-1234-123456789ABD\nName: Preboot\nFileVault: No\n")
OTHER = "Volume 0 12345678-1234-1234-1234-123456789ABE\nName: Data\nFileVault: No\n"


def lsblk(*devices):
    return {"blockdevices": [{"path": d, "fstype": "apfs", "mountpoints": m} for d, m in devices]}


def make_dmg(preboot, uuid=UUID_A):
    image = preboot / "root" / uuid / "cryptex1/current/os.dmg"
    image.parent.mkdir(parents=True)
    image.write_text("fixture")
    return image


class ArgumentTest(unittest.TestCase):
    def test_preboot_arguments_are_one_argv_list(self):
        self.assertEqual(
            cryptex.preboot_arguments("/opt/apfs fuse", 1000, 1001, 3, "/dev/nvme0n1p3", Path("/tmp/m r/macos-preboot")),
            ["/opt/apfs fuse", "-o", "allow_other,uid=1000,gid=1001", "-v", "3", "/dev/nvme0n1p3", "/tmp/m r/macos-preboot"])

    def test_cryptex_arguments_have_no_allow_other(self):
        arguments = cryptex.cryptex_arguments("/opt/fuse", 1000, 1001, Path("/p/os.dmg"), Path("/m/macos-cryptex-os"))
        self.assertEqual(arguments, ["/opt/fuse", "-o", "uid=1000,gid=1001", "-v", "0", "/p/os.dmg", "/m/macos-cryptex-os"])
        self.assertNotIn("allow_other", " ".join(arguments))

    def test_refusals(self):
        for uid in (0, -1, "1000"):
            with self.assertRaises(ValueError):
                cryptex.preboot_arguments("/f", uid, 0, 3, "/dev/sda2", Path("/m"))
            with self.assertRaises(ValueError):
                cryptex.cryptex_arguments("/f", uid, 0, Path("/p/os.dmg"), Path("/m"))
        for index in (-1, 100, "3"):
            with self.assertRaises(ValueError):
                cryptex.preboot_arguments("/f", 1000, 1000, index, "/dev/sda2", Path("/m"))
        for device in ("/dev/sda2;rm -rf /", "sda2", "/dev/../etc/passwd", "/dev/a b"):
            with self.assertRaises(ValueError):
                cryptex.preboot_arguments("/f", 1000, 1000, 3, device, Path("/m"))
        with self.assertRaises(ValueError):
            cryptex.cryptex_arguments("/f", 1000, 1000, Path("relative/os.dmg"), Path("/m"))


class DiscoveryTest(unittest.TestCase):
    def find(self, inventory, listings, existing=(), pinned=None):
        return cryptex.find_preboot(inventory, lambda argv: listings[argv[1]], "/util", list(existing), lambda _: None, pinned)

    def test_preboot_index_is_discovered_not_hardcoded(self):
        self.assertEqual(self.find(lsblk(("/dev/sda2", [])), {"/dev/sda2": LIST}), ("/dev/sda2", 3))
        moved = LIST.replace("Volume 3", "Volume 5")
        self.assertEqual(self.find(lsblk(("/dev/sda2", [])), {"/dev/sda2": moved}), ("/dev/sda2", 5))

    def test_no_preboot_is_refused(self):
        with self.assertRaisesRegex(ValueError, "No unmounted"):
            self.find(lsblk(("/dev/sda2", [])), {"/dev/sda2": OTHER})
        with self.assertRaisesRegex(ValueError, "No unmounted"):
            self.find({"blockdevices": []}, {})

    def test_mounted_partitions_are_skipped(self):
        with self.assertRaisesRegex(ValueError, "No unmounted"):
            self.find(lsblk(("/dev/sda2", ["/mnt"])), {"/dev/sda2": LIST})
        with self.assertRaisesRegex(ValueError, "No unmounted"):
            self.find(lsblk(("/dev/sda2", [])), {"/dev/sda2": LIST}, existing=[{"source": "/dev/sda2", "target": "/x"}])

    def test_ambiguity_is_refused_unless_pinned(self):
        inventory, listings = lsblk(("/dev/sda2", []), ("/dev/sdb2", [])), {"/dev/sda2": LIST, "/dev/sdb2": LIST}
        with self.assertRaisesRegex(ValueError, "Ambiguous"):
            self.find(inventory, listings)
        self.assertEqual(self.find(inventory, listings, pinned="/dev/sdb2"), ("/dev/sdb2", 3))
        with self.assertRaisesRegex(ValueError, "not a detected"):
            self.find(inventory, listings, pinned="/dev/sdc2")

    def test_two_preboot_volumes_on_one_partition_are_ambiguous(self):
        with self.assertRaisesRegex(ValueError, "Ambiguous"):
            self.find(lsblk(("/dev/sda2", [])), {"/dev/sda2": LIST + LIST.replace("Volume 3", "Volume 4")})

    def test_invalid_device_aborts(self):
        with self.assertRaises(ValueError):
            self.find(lsblk(("/dev/sda2;x", [])), {})

    def test_os_dmg_uuid_discovery(self):
        with tempfile.TemporaryDirectory() as directory:
            preboot = Path(directory)
            (preboot / "root/not-a-uuid").mkdir(parents=True)
            image = make_dmg(preboot)
            self.assertEqual(cryptex.find_os_dmg(preboot), image.resolve())

    def test_os_dmg_refusals(self):
        with tempfile.TemporaryDirectory() as directory:
            preboot = Path(directory)
            with self.assertRaisesRegex(ValueError, "missing"):
                cryptex.find_os_dmg(preboot)
            (preboot / "root" / UUID_A).mkdir(parents=True)
            with self.assertRaisesRegex(ValueError, "No <UUID>"):
                cryptex.find_os_dmg(preboot)
            make_dmg(preboot, UUID_A)
            make_dmg(preboot, UUID_B)
            with self.assertRaisesRegex(ValueError, "Ambiguous"):
                cryptex.find_os_dmg(preboot)

    def test_os_dmg_symlink_escape_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            preboot, outside = Path(directory) / "preboot", Path(directory) / "outside"
            (preboot / "root").mkdir(parents=True)
            outside.mkdir()
            (outside / "os.dmg").write_text("fixture")
            (preboot / "root" / UUID_A).symlink_to(outside, target_is_directory=True)
            (outside / "cryptex1/current").mkdir(parents=True)
            (outside / "cryptex1/current/os.dmg").write_text("fixture")
            with self.assertRaisesRegex(ValueError, "outside"):
                cryptex.find_os_dmg(preboot)


class MountTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        for name in ("util", "fuse"):
            (self.root / name).write_text("fixture")
            (self.root / name).chmod(0o700)
        self.options = SimpleNamespace(mount_root=str(self.root / "mounts"), apfsutil=str(self.root / "util"),
                                       apfs_fuse=str(self.root / "fuse"), device="")
        self.calls, self.records, self.events = [], [], []

    def emit(self, kind, **fields):
        self.events.append({"event": kind, **fields})

    def fake_run(self, argv):
        self.calls.append(argv)
        if argv[0] == "/usr/bin/lsblk":
            return json.dumps(lsblk(("/dev/synthetic2", [])))
        if argv[0] == "/usr/bin/findmnt":
            return json.dumps({"filesystems": self.records})
        if argv[0] == self.options.apfsutil:
            return LIST
        self.assertEqual(argv[0], self.options.apfs_fuse)
        self.records.append({"source": argv[-2], "target": argv[-1], "options": "rw"})
        if "allow_other" not in argv[2]:
            dyld = Path(argv[-1]) / "root/System/Library/dyld"
            dyld.mkdir(parents=True)
        return ""

    def test_preboot_step_builds_exact_argv_and_verifies(self):
        destination = cryptex.mount_preboot(self.options, 1000, 1000, self.fake_run, self.emit, lambda _: None)
        fuse = [c for c in self.calls if c[0] == self.options.apfs_fuse]
        self.assertEqual(fuse, [[self.options.apfs_fuse, "-o", "allow_other,uid=1000,gid=1000", "-v", "3", "/dev/synthetic2", str(destination)]])
        self.assertEqual(destination, self.root / "mounts/macos-preboot")
        self.assertEqual(self.events[-1], {"event": "mounted", "role": "preboot", "path": str(destination), "device": "/dev/synthetic2"})

    def test_preboot_mount_that_does_not_verify_fails(self):
        def run(argv):
            return "" if argv[0] == self.options.apfs_fuse else self.fake_run(argv)
        with self.assertRaisesRegex(RuntimeError, "did not verify"):
            cryptex.mount_preboot(self.options, 1000, 1000, run, self.emit, lambda _: None)

    def test_missing_tools_fail_loudly(self):
        self.options.apfs_fuse = str(self.root / "absent")
        with self.assertRaisesRegex(ValueError, "Missing executable"):
            cryptex.mount_preboot(self.options, 1000, 1000, self.fake_run, self.emit, lambda _: None)
        self.assertFalse(any(c[0] == self.options.apfs_fuse for c in self.calls))

    def test_cryptex_step_refuses_without_preboot(self):
        with self.assertRaisesRegex(ValueError, "Preboot volume is not mounted"):
            cryptex.mount_cryptex(self.options, 1000, 1000, self.fake_run, self.emit, lambda _: None)

    def test_cryptex_step_refuses_as_root(self):
        with patch.object(cryptex.os, "geteuid", return_value=0), self.assertRaisesRegex(ValueError, "never as root"):
            cryptex.mount_cryptex(self.options, 1000, 1000, self.fake_run, self.emit, lambda _: None)

    def test_cryptex_step_mounts_dmg_and_reports_dyld(self):
        preboot = self.root / "mounts/macos-preboot"
        image = make_dmg(preboot)
        self.records.append({"source": "/dev/synthetic2", "target": str(preboot)})
        dyld = cryptex.mount_cryptex(self.options, 1000, 1000, self.fake_run, self.emit, lambda _: None)
        fuse = [c for c in self.calls if c[0] == self.options.apfs_fuse]
        self.assertEqual(fuse, [[self.options.apfs_fuse, "-o", "uid=1000,gid=1000", "-v", "0", str(image.resolve()), str(self.root / "mounts/macos-cryptex-os")]])
        self.assertEqual(dyld, self.root / "mounts/macos-cryptex-os/root/System/Library/dyld")
        kinds = [e["event"] for e in self.events]
        self.assertLess(kinds.index("warning"), kinds.index("mounted"))
        self.assertEqual(kinds[-1], "dyld")
        self.assertIn("GB", self.events[kinds.index("warning")]["message"])

    def test_already_mounted_cryptex_is_not_mounted_twice(self):
        preboot, target = self.root / "mounts/macos-preboot", self.root / "mounts/macos-cryptex-os"
        (target / "root/System/Library/dyld").mkdir(parents=True)
        self.records += [{"target": str(preboot)}, {"target": str(target)}]
        cryptex.mount_cryptex(self.options, 1000, 1000, self.fake_run, self.emit, lambda _: None)
        self.assertFalse(any(c[0] == self.options.apfs_fuse for c in self.calls))

    def test_missing_dyld_directory_fails_after_waiting(self):
        preboot = self.root / "mounts/macos-preboot"
        make_dmg(preboot)
        self.records.append({"target": str(preboot)})
        sleeps = []
        def run(argv):
            if argv[0] == self.options.apfs_fuse:
                self.records.append({"target": argv[-1]})
                return ""
            return self.fake_run(argv)
        with self.assertRaisesRegex(RuntimeError, "did not appear"):
            cryptex.mount_cryptex(self.options, 1000, 1000, run, self.emit, sleeps.append)
        self.assertEqual(len(sleeps), cryptex.DYLD_WAIT_SECONDS)


class MainTest(unittest.TestCase):
    ARGS = ["--mount-root", "/tmp/path with spaces", "--apfs-fuse", "/tmp/fuse", "--apfsutil", "/tmp/util"]

    def test_privileged_mode_requires_pkexec_identity(self):
        with patch.object(cryptex.base.os, "geteuid", return_value=1000), patch.object(cryptex.base, "emit") as emit:
            self.assertEqual(cryptex.main(["--privileged", *self.ARGS]), 1)
            self.assertIn("pkexec", emit.call_args.kwargs["message"])

    def test_one_authorization_then_cryptex_step(self):
        with patch.object(cryptex.shutil, "which", return_value="/usr/bin/pkexec"), \
             patch.object(cryptex.base, "mounted_records", return_value=[]), \
             patch.object(cryptex.subprocess, "call", return_value=0) as call, \
             patch.object(cryptex, "mount_cryptex") as step:
            self.assertEqual(cryptex.main([*self.ARGS, "--device", "/dev/sda2"]), 0)
            self.assertEqual(call.call_count, 1)
            command = call.call_args.args[0]
            self.assertEqual(command[0], "/usr/bin/pkexec")
            self.assertEqual(command[command.index("--device") + 1], "/dev/sda2")
            self.assertIn("--privileged", command)
            self.assertIn("/tmp/path with spaces", command)
            self.assertEqual(step.call_count, 1)

    def test_cancel_or_denial_stops_without_retry_or_cryptex_step(self):
        for code in (1, 126, 127):
            with patch.object(cryptex.shutil, "which", return_value="/usr/bin/pkexec"), \
                 patch.object(cryptex.base, "mounted_records", return_value=[]), \
                 patch.object(cryptex.subprocess, "call", return_value=code) as call, \
                 patch.object(cryptex, "mount_cryptex") as step, patch.object(cryptex.base, "emit"):
                self.assertEqual(cryptex.main(self.ARGS), code)
                self.assertEqual(call.call_count, 1)
                step.assert_not_called()

    def test_mounted_preboot_skips_authorization(self):
        records = [{"target": "/tmp/path with spaces/macos-preboot"}]
        with patch.object(cryptex.base, "mounted_records", return_value=records), \
             patch.object(cryptex.subprocess, "call") as call, patch.object(cryptex, "mount_cryptex") as step:
            self.assertEqual(cryptex.main(self.ARGS), 0)
            call.assert_not_called()
            self.assertEqual(step.call_count, 1)

    def test_missing_pkexec_fails_loudly(self):
        with patch.object(cryptex.shutil, "which", return_value=None), patch.object(cryptex.base, "mounted_records", return_value=[]), \
             patch.object(cryptex.base, "emit") as emit:
            self.assertEqual(cryptex.main(self.ARGS), 1)
            self.assertIn("pkexec", emit.call_args.kwargs["message"])


if __name__ == "__main__":
    unittest.main()
