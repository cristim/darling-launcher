#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import errno
import importlib.util
import json
import os
from pathlib import Path
import stat
import struct
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch
from contextlib import contextmanager

spec = importlib.util.spec_from_file_location("mount_macos", Path(__file__).resolve().parents[1] / "tools/mount-macos.py")
mount = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mount)

UID, GID = os.getuid(), os.getgid()
VOLUMES = "Volume 0 12345678-1234-1234-1234-123456789ABC\nName: macOS\nFileVault: No\nVolume 1 12345678-1234-1234-1234-123456789ABC\nFileVault: Yes\n"


CHOWNS = []


@contextmanager
def as_root():
    # Tests cannot create root-owned directories, so the test user stands in for root and
    # fchown is recorded instead of performed.
    CHOWNS.clear()
    with patch.object(mount, "root_owned", lambda status: status.st_uid in (0, UID) and not status.st_mode & 0o022), \
         patch.object(mount.os, "fchown", lambda fd, uid, gid: CHOWNS.append((os.fstat(fd).st_ino, uid, gid))):
        yield


class Fixture:
    """A temporary root with fake tools, a fake lsblk/findmnt/mount and a parent for mount points."""

    def __init__(self, directory, fstype="apfs"):
        self.root = Path(directory)
        (self.root / "run").mkdir()
        self.parent = str(self.root / "run/darling-launcher")
        self.records, self.events, self.calls = [], [], []
        self.fstype = fstype
        for name in ("util", "fuse"):
            (self.root / name).write_text("fixture")
            (self.root / name).chmod(0o755)
        self.mount_options = "ro,nodev,nosuid,noexec"
        self.before_mount = None
        self.mount_source = None
        self.mount_exit = 0
        self.findmnt_broken = False
        self.umount_failures = 0
        self.lsblk_path = "/dev/synthetic"

    def tools(self):
        return patch.dict(mount.APFS_TOOLS, {"apfsutil": (str(self.root / "util"),), "apfs-fuse": (str(self.root / "fuse"),)})

    def run(self, arguments):
        self.calls.append(arguments)
        if arguments[0] == "/usr/bin/lsblk":
            return json.dumps({"blockdevices": [{"path": self.lsblk_path, "fstype": self.fstype, "mountpoints": []},
                                                {"path": "/dev/already", "fstype": "apfs", "mountpoints": ["/existing"]}]})
        if arguments[0] == "/usr/bin/findmnt":
            if self.findmnt_broken and self.mounts():
                raise RuntimeError("findmnt failed with exit status 1")
            return json.dumps({"filesystems": self.records})
        if arguments[0] == str(self.root / "util"):
            return VOLUMES
        if arguments[0] == "/usr/bin/umount":
            if self.umount_failures:
                self.umount_failures -= 1
                raise RuntimeError("umount failed with exit status 32")
            self.records[:] = [r for r in self.records if r["target"] != arguments[-1]]
            return ""
        if self.before_mount:
            self.before_mount(Path(arguments[-1]))
        # Like mount(2), resolve the target path at mount time.
        target = os.path.realpath(arguments[-1])
        (Path(target) / "System/Library").mkdir(parents=True, exist_ok=True)
        (Path(target) / "System/Applications").mkdir(exist_ok=True)
        self.records.append({"source": self.mount_source or arguments[-2], "target": target, "options": self.mount_options})
        if self.mount_exit:
            raise RuntimeError(f"{Path(arguments[0]).name} failed with exit status {self.mount_exit}")
        return ""

    def batch(self, options=None, check_device=lambda _: None):
        with as_root(), patch.object(mount, "trusted", side_effect=lambda path: path), self.tools():
            return mount.mount_batch(options or SimpleNamespace(), UID, GID, self.run,
                                     lambda kind, **fields: self.events.append({"event": kind, **fields}), check_device, parent=self.parent)

    def errors(self):
        return [e["message"] for e in self.events if e["event"] == "error"]

    def mounts(self):
        return [c for c in self.calls if c[0] in (str(self.root / "fuse"), "/usr/bin/mount")]


class MountScriptTest(unittest.TestCase):
    def test_one_authorization_and_no_retry(self):
        for code in (0, 126, 127):
            with patch.object(mount, "trusted", side_effect=lambda path: path), patch.object(mount.subprocess, "call", return_value=code) as call, patch.object(mount, "emit"):
                self.assertEqual(mount.main([]), code)
                self.assertEqual(call.call_count, 1)
                self.assertEqual(call.call_args.args[0], ["/usr/bin/pkexec", "/usr/bin/python3", "-I", mount.__file__, "--privileged"])

    def test_metadata_encryption_and_invalid_indices(self):
        result = mount.volumes("Volume 0 12345678-1234-1234-1234-123456789ABC\nName: macOS\nFileVault: No\nVolume 1 12345678-1234-1234-1234-123456789ABC\nFileVault: Yes\nVolume 100 12345678-1234-1234-1234-123456789ABC\nFileVault: No\n")
        self.assertEqual(len(result), 2)
        self.assertFalse(result[0]["encrypted"])
        self.assertTrue(result[1]["encrypted"])

    def test_readonly_batch_under_the_root_owned_parent(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(directory)
            owned = fixture.batch()
            expected = str(Path(fixture.parent) / str(UID) / "synthetic-volume-0")
            self.assertEqual(owned, [expected])
            self.assertEqual(fixture.events[-1]["sources"], owned)
            self.assertEqual(len(fixture.mounts()), 1)
            self.assertEqual(fixture.mounts()[0][1:7], ["-v", "0", "-o", f"ro,nodev,nosuid,noexec,uid={UID},gid={GID},allow_other,default_permissions,fsname=/dev/synthetic", "--", "/dev/synthetic"])
            self.assertTrue(any(e.get("reason", "").startswith("Encrypted") for e in fixture.events))
            self.assertTrue(any(e.get("reason") == "Already mounted" for e in fixture.events))

    def test_writable_mount_is_unmounted(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(directory)
            fixture.mount_options = "rw,nodev,nosuid,noexec"
            self.assertEqual(fixture.batch(), [])
            self.assertEqual(fixture.records, [])
            self.assertTrue(any(e["event"] == "error" and "verify" in e["message"] for e in fixture.events))

    def test_mount_parent_that_others_can_write_is_refused(self):
        # With the real check, a parent owned by the (non-root) test user is exactly what must be refused.
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(directory)
            with patch.object(mount, "trusted", side_effect=lambda path: path), fixture.tools(), self.assertRaises(ValueError):
                mount.mount_batch(SimpleNamespace(), UID, GID, fixture.run, lambda *a, **k: None, lambda _: None, parent=fixture.parent)
            self.assertEqual(fixture.mounts(), [])
            (fixture.root / "run").chmod(0o777)
            with as_root(), self.assertRaises(ValueError):
                mount.open_mount_parent(UID, GID, fixture.parent)
            self.assertFalse(Path(fixture.parent).exists())

    def test_symlink_in_the_mount_parent_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "real").mkdir()
            (root / "run").symlink_to(root / "real", target_is_directory=True)
            with as_root(), self.assertRaises(ValueError):
                mount.open_mount_parent(UID, GID, str(root / "run/darling-launcher"))
            self.assertEqual(list((root / "real").iterdir()), [])

    def test_mount_directory_swapped_before_mounting_is_not_mounted(self):
        # Only possible if someone can write the parent, which the root-owned parent rules out;
        # the identity check still refuses to mount onto anything but the prepared directory.
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(directory)
            victim = fixture.root / "system-directory"
            victim.mkdir()
            real = mount.mount_directory
            def swapping(parent_fd, name):
                descriptor = real(parent_fd, name)
                os.rename(name, name + "-moved", src_dir_fd=parent_fd, dst_dir_fd=parent_fd)
                os.symlink(victim, name, dir_fd=parent_fd)
                return descriptor
            with patch.object(mount, "mount_directory", swapping):
                self.assertEqual(fixture.batch(), [])
            self.assertEqual(fixture.mounts(), [])
            self.assertTrue(any(e["event"] == "error" and "changed" in e["message"] for e in fixture.events))

    def test_foreign_mount_of_the_device_is_never_unmounted(self):
        # Anyone can create a mount record naming the device (FUSE fsname=); root unmounts only its own target.
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(directory)
            foreign = str(fixture.root / "user-chosen")
            fixture.mount_exit = 1
            def foreign_mount(target):
                fixture.records.append({"source": "/dev/synthetic", "target": foreign, "options": "rw"})
            fixture.before_mount = foreign_mount
            self.assertEqual(fixture.batch(), [])
            umounts = [c for c in fixture.calls if c[0] == "/usr/bin/umount"]
            self.assertTrue(umounts)
            self.assertTrue(all(c[-1] == str(Path(fixture.parent) / str(UID) / "synthetic-volume-0") for c in umounts), umounts)
            self.assertIn(foreign, [r["target"] for r in fixture.records])

    def test_single_partition_goes_through_the_same_helper(self):
        with patch.object(mount, "trusted", side_effect=lambda path: path), patch.object(mount.subprocess, "call", return_value=0) as call:
            mount.main(["--device", "/dev/synthetic", "--volume", "0", "--kernel"])
        self.assertEqual(call.call_args.args[0][4:], ["--privileged", "--device=/dev/synthetic", "--volume=0", "--kernel"])
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(directory)
            owned = fixture.batch(SimpleNamespace(device="/dev/synthetic", volume=0, kernel=False))
            self.assertEqual(owned, [str(Path(fixture.parent) / str(UID) / "synthetic-volume-0")])
            with self.assertRaisesRegex(ValueError, "not a detected"):
                fixture.batch(SimpleNamespace(device="/dev/sda1", volume=None, kernel=False))

    def test_kernel_apfs_needs_an_explicit_index(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(directory)
            owned = fixture.batch(SimpleNamespace(device="/dev/synthetic", volume=2, kernel=True))
            destination = str(Path(fixture.parent) / str(UID) / "synthetic-volume-2")
            self.assertEqual(owned, [destination])
            self.assertEqual(fixture.mounts(), [["/usr/bin/mount", "-i", "-t", "apfs", "-o", f"ro,nodev,nosuid,noexec,uid={UID},gid={GID},vol=2", "--", "/dev/synthetic", destination]])
            self.assertNotIn(str(fixture.root / "util"), [c[0] for c in fixture.calls])
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(directory)
            self.assertEqual(fixture.batch(SimpleNamespace(device="/dev/synthetic", volume=None, kernel=True)), [])
            self.assertTrue(any("index explicitly" in e.get("message", "") for e in fixture.events))

    def test_tool_paths_from_the_caller_are_never_run(self):
        # Settings and argv are writable by anything running as the user; a root-owned but wrong
        # binary (a shell, an interpreter) given as apfsutil would run as root on an attacker image.
        for argv in (["--apfsutil", "/usr/bin/sh"], ["--privileged", "--apfs-fuse", "/usr/bin/sh"]):
            with patch.object(mount.subprocess, "call") as call, patch("sys.stderr"), self.assertRaises(SystemExit):
                mount.main(argv)
            call.assert_not_called()
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(directory)
            fixture.batch(SimpleNamespace(apfsutil="/usr/bin/sh", apfs_fuse="/usr/bin/sh"))
            programs = [c[0] for c in fixture.calls]
            self.assertNotIn("/usr/bin/sh", programs)
            self.assertIn(str(fixture.root / "util"), programs)
            self.assertIn(str(fixture.root / "fuse"), programs)

    def test_apfs_tools_come_only_from_the_fixed_list(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.dict(mount.APFS_TOOLS, {"apfsutil": (os.path.join(directory, "missing"), "/usr/bin/true")}):
                self.assertEqual(mount.apfs_tool("apfsutil"), os.path.realpath("/usr/bin/true"))
            with patch.dict(mount.APFS_TOOLS, {"apfsutil": (os.path.join(directory, "missing"),)}), self.assertRaisesRegex(ValueError, "not installed"):
                mount.apfs_tool("apfsutil")
        self.assertEqual(mount.APFS_TOOLS["apfs-fuse"], ("/usr/bin/apfs-fuse", "/usr/local/bin/apfs-fuse"))

    def test_authorization_runs_only_pinned_root_owned_programs(self):
        # A pkexec and python3 earlier on PATH, as anything running as the user could arrange,
        # must not be what the authorization runs.
        with tempfile.TemporaryDirectory() as directory:
            for name in ("pkexec", "python3"):
                fake = Path(directory) / name
                fake.write_text("#!/bin/sh\nexit 0\n")
                fake.chmod(0o755)
            script = Path(directory) / "installed-mount-macos.py"
            with patch.dict(os.environ, {"PATH": directory + ":" + os.environ.get("PATH", "")}), patch.object(mount, "__file__", str(script)), patch.object(mount, "trusted", side_effect=lambda path: path) as check:
                command = mount.authorization_command(SimpleNamespace(device=None, volume=None, kernel=False))
            self.assertEqual(command[:4], ["/usr/bin/pkexec", "/usr/bin/python3", "-I", str(script)])
            self.assertEqual([c.args[0] for c in check.call_args_list], ["/usr/bin/pkexec", "/usr/bin/python3", str(script)])

    def test_user_writable_script_is_never_authorized(self):
        # This test file lives in a user-owned checkout, and so does the script next to it.
        with patch.object(mount.subprocess, "call") as call, patch.object(mount, "emit") as emitted:
            self.assertEqual(mount.main([]), 1)
        call.assert_not_called()
        self.assertIn("Refusing to run", emitted.call_args.kwargs["message"])

    def test_privileged_helpers_must_be_root_owned(self):
        with tempfile.TemporaryDirectory() as directory:
            helper = Path(directory) / "apfs-fuse"
            helper.write_text("#!/bin/sh\nexit 0\n")
            helper.chmod(0o755)
            with self.assertRaisesRegex(ValueError, "Refusing to run"):
                mount.trusted(str(helper))
            for relative in ("apfs-fuse", "./apfs-fuse", ""):
                with self.assertRaises(ValueError):
                    mount.trusted(relative)
            link = Path(directory) / "link-to-true"
            link.symlink_to("/usr/bin/true")
            self.assertEqual(mount.trusted(str(link)), os.path.realpath("/usr/bin/true"))
            self.assertEqual(mount.trusted("/usr/bin/true"), os.path.realpath("/usr/bin/true"))
            with self.assertRaisesRegex(ValueError, "Not an executable"):
                mount.trusted("/etc/hostname" if os.path.isfile("/etc/hostname") else "/etc/passwd")

    def test_privileged_batch_refuses_user_owned_apfs_tools(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(directory)
            with as_root(), fixture.tools():
                self.assertEqual(mount.mount_batch(SimpleNamespace(), UID, GID, fixture.run, lambda kind, **fields: fixture.events.append({"event": kind, **fields}), lambda _: None, parent=fixture.parent), [])
            programs = [c[0] for c in fixture.calls]
            self.assertNotIn(str(fixture.root / "util"), programs)
            self.assertNotIn(str(fixture.root / "fuse"), programs)
            self.assertTrue(any("Refusing to run" in e.get("message", "") for e in fixture.events))

    def test_created_directories_get_explicit_restrictive_modes(self):
        # Created under a permissive umask, every level still ends with exactly its intended mode and owner.
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(directory)
            previous = os.umask(0)
            try:
                fixture.batch()
            finally:
                os.umask(previous)
            parent, per_user = Path(fixture.parent), Path(fixture.parent) / str(UID)
            volume = per_user / "synthetic-volume-0"
            modes = {path: stat.S_IMODE(os.stat(path).st_mode) for path in (parent, per_user, volume)}
            self.assertEqual(modes, {parent: 0o755, per_user: 0o750, volume: 0o700})
            owners = {inode: (uid, gid) for inode, uid, gid in CHOWNS}
            self.assertEqual(owners, {os.stat(parent).st_ino: (0, 0), os.stat(per_user).st_ino: (0, 0), os.stat(volume).st_ino: (0, 0)})

    def test_only_the_caller_can_enter_the_per_user_directory(self):
        # Mounts carry gid=<primary group>, which may be shared; the ACL keeps group members out.
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(directory)
            try:
                os.setxattr(directory, "system.posix_acl_access", mount.caller_only_acl(UID))
            except OSError:
                self.skipTest("no POSIX ACL support for the test directory")
            fixture.batch()
            value = os.getxattr(Path(fixture.parent) / str(UID), "system.posix_acl_access")
            entries = {(tag, perm, ident) for tag, perm, ident in struct.iter_unpack("<HHI", value[4:])}
            self.assertEqual(entries, {(0x01, 7, 0xFFFFFFFF), (0x02, 5, UID), (0x04, 0, 0xFFFFFFFF), (0x10, 5, 0xFFFFFFFF), (0x20, 0, 0xFFFFFFFF)})
            self.assertFalse(any(e["event"] == "warning" for e in fixture.events))

    def test_without_acl_support_the_primary_group_is_used_and_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(directory)
            with patch.object(mount.os, "setxattr", side_effect=OSError(errno.EOPNOTSUPP, "Operation not supported")):
                self.assertEqual(len(fixture.batch()), 1)
            per_user = Path(fixture.parent) / str(UID)
            self.assertIn((os.stat(per_user).st_ino, 0, GID), CHOWNS)
            self.assertTrue(any(e["event"] == "warning" and "primary group" in e["message"] for e in fixture.events))

    def test_privileged_branch_sets_a_private_umask(self):
        with patch.object(mount.os, "geteuid", return_value=0), patch.dict(os.environ, {"PKEXEC_UID": str(UID or 1000)}), \
             patch.object(mount, "mount_batch") as batch, patch.object(mount.os, "umask") as umask:
            self.assertEqual(mount.main(["--privileged"]), 0)
        umask.assert_called_once_with(0o077)
        batch.assert_called_once()

    def test_errors_do_not_disclose_paths_or_tool_output(self):
        with tempfile.TemporaryDirectory() as directory:
            secret = Path(directory) / "secret-name"
            message = mount.describe(FileNotFoundError(2, "No such file or directory", str(secret)))
            self.assertEqual(message, "No such file or directory")
            with self.assertRaises(RuntimeError) as raised:
                mount.run(["/bin/sh", "-c", f"echo {secret}; exit 3"])
            self.assertEqual(str(raised.exception), "sh failed with exit status 3")

    def test_only_this_runs_mounts_are_reported_as_sources(self):
        # Root must not probe other users' or unrelated mounts for a macOS layout on the caller's behalf.
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(directory)
            elsewhere = fixture.root / "elsewhere"
            (elsewhere / "System/Library").mkdir(parents=True)
            (elsewhere / "Applications").mkdir()
            fixture.records.append({"source": "/dev/other", "target": str(elsewhere), "options": "ro"})
            owned = fixture.batch()
            self.assertEqual(fixture.events[-1]["sources"], owned)
            self.assertNotIn(str(elsewhere), fixture.events[-1]["sources"])

    def test_mount_recorded_under_another_source_is_unmounted(self):
        # A FUSE mount that does not show the device as its source is not verified, and still comes off.
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(directory)
            fixture.mount_source = "apfs-fuse"
            self.assertEqual(fixture.batch(), [])
            self.assertEqual(fixture.records, [])
            self.assertTrue(any(e["event"] == "error" and "unmounted" in e["message"] for e in fixture.events))

    def test_failed_mount_tool_leaves_nothing_mounted(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(directory)
            fixture.mount_exit = 1
            self.assertEqual(fixture.batch(), [])
            self.assertEqual(fixture.records, [])
            self.assertTrue(any(e["event"] == "error" and "exit status 1; unmounted" in e["message"] for e in fixture.events))

    def test_selection_options_are_validated(self):
        for argv in (["--device="], ["--volume=0"], ["--kernel"], ["--device=/dev/x", "--volume=100"], ["--device=/dev/x", "--volume=-1"]):
            with patch.object(mount.subprocess, "call") as call, patch("sys.stderr"), self.assertRaises(SystemExit):
                mount.main(argv)
            call.assert_not_called()

    def test_unverified_mount_comes_off_when_findmnt_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(directory)
            fixture.findmnt_broken = True
            self.assertEqual(fixture.batch(), [])
            self.assertEqual(fixture.records, [])
            self.assertTrue(any("findmnt failed" in m and "unmounted" in m for m in fixture.errors()), fixture.errors())

    def test_busy_target_is_lazily_unmounted_and_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(directory)
            fixture.mount_options = "rw,nodev,nosuid,noexec"
            fixture.umount_failures = 1
            self.assertEqual(fixture.batch(), [])
            self.assertEqual(fixture.records, [])
            target = str(Path(fixture.parent) / str(UID) / "synthetic-volume-0")
            self.assertIn(["/usr/bin/umount", "-l", "--", target], fixture.calls)
            self.assertTrue(any("read-only" in m and "unmounted" in m for m in fixture.errors()), fixture.errors())
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(directory)
            fixture.mount_options = "rw,nodev,nosuid,noexec"
            fixture.umount_failures = 2
            fixture.batch()
            self.assertTrue(any("could not be unmounted" in m for m in fixture.errors()), fixture.errors())

    def test_undetectable_device_paths_are_refused(self):
        for path in ("/dev/x;y", "/dev/../sda", "dev/sda", "/dev/x,uid=0"):
            with tempfile.TemporaryDirectory() as directory:
                fixture = Fixture(directory, fstype="hfsplus")
                fixture.lsblk_path = path
                self.assertEqual(fixture.batch(), [])
                self.assertEqual(fixture.mounts(), [])
                self.assertIn("Invalid detected device path", fixture.errors())

    def test_non_block_device_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(directory, fstype="hfsplus")
            fixture.lsblk_path = "/dev/null"
            self.assertEqual(fixture.batch(check_device=None), [])
            self.assertEqual(fixture.mounts(), [])
            self.assertIn("Detected path is not a block device", fixture.errors())

    def test_target_already_mounted_is_not_mounted_over(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(directory)
            target = str(Path(fixture.parent) / str(UID) / "synthetic-volume-0")
            fixture.records.append({"source": "/dev/elsewhere", "target": target, "options": "ro"})
            self.assertEqual(fixture.batch(), [])
            self.assertEqual(fixture.mounts(), [])
            self.assertIn("Mount directory is mounted or not empty", fixture.errors())

    def test_non_empty_volume_directory_is_not_mounted_over(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Fixture(directory)
            volume = Path(fixture.parent) / str(UID) / "synthetic-volume-0"
            volume.mkdir(parents=True)
            (volume / "leftover").write_text("")
            self.assertEqual(fixture.batch(), [])
            self.assertEqual(fixture.mounts(), [])
            self.assertIn("Mount directory is mounted or not empty", fixture.errors())

    def test_caller_without_passwd_entry_gets_a_clear_error(self):
        with patch.object(mount.os, "geteuid", return_value=0), patch.dict(os.environ, {"PKEXEC_UID": "4000000000"}), \
             patch.object(mount.pwd, "getpwuid", side_effect=KeyError("getpwuid(): uid not found")), \
             patch.object(mount, "mount_batch") as batch, patch.object(mount, "emit") as emitted:
            self.assertEqual(mount.main(["--privileged"]), 1)
        batch.assert_not_called()
        self.assertEqual(emitted.call_args.kwargs["message"], "The calling user has no passwd entry")

    def test_privileged_mode_requires_pkexec_identity(self):
        with patch.object(mount.os, "geteuid", return_value=1000), patch.object(mount, "emit"):
            self.assertEqual(mount.main(["--privileged"]), 1)

if __name__ == "__main__":
    unittest.main()
