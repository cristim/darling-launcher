#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
import json
import os
from pathlib import Path
import stat
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


UID = os.getuid()


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


def fake_lstat(overrides=None):
    """Pretend everything on disk is root-owned and 0755 except the listed (uid, group_writable) paths."""
    overrides = overrides or {}
    def lstat(path):
        real = os.lstat(path)
        uid, writable = overrides.get(str(path), (0, False))
        if stat.S_ISLNK(real.st_mode):  # Linux reports 0777 for symlinks
            return SimpleNamespace(st_uid=uid, st_mode=real.st_mode)
        return SimpleNamespace(st_uid=uid, st_mode=(real.st_mode & ~0o777) | (0o775 if writable else 0o755))
    return lstat


class TrustTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(os.path.realpath(self.tmp.name))
        self.bin = self.root / "usr/local/bin"
        self.bin.mkdir(parents=True)
        self.tool = self.bin / "apfs-fuse"
        self.tool.write_text("fixture")
        self.tool.chmod(0o755)

    def find(self, overrides=None, name="apfs-fuse", directories=None):
        return cryptex.system_tool(name, directories or [str(self.bin)], fake_lstat(overrides))

    def test_root_owned_tool_is_accepted(self):
        self.assertEqual(self.find(), str(self.tool))

    def test_user_owned_binary_is_refused(self):
        with self.assertRaisesRegex(ValueError, "owned by root"):
            self.find({str(self.tool): (1000, False)})

    def test_group_writable_parent_is_refused(self):
        with self.assertRaisesRegex(ValueError, "not group- or world-writable"):
            self.find({str(self.bin): (0, True)})
        with self.assertRaisesRegex(ValueError, "not group- or world-writable"):
            self.find({str(self.root / "usr"): (0, True)})

    def test_user_owned_ancestor_is_refused(self):
        with self.assertRaisesRegex(ValueError, "owned by root"):
            self.find({str(self.root): (1000, False)})

    def test_symlink_to_untrusted_target_is_refused(self):
        elsewhere = self.root / "home/me"
        elsewhere.mkdir(parents=True)
        (elsewhere / "evil").write_text("fixture")
        (elsewhere / "evil").chmod(0o755)
        (self.bin / "apfsutil").symlink_to(elsewhere / "evil")
        with self.assertRaisesRegex(ValueError, "owned by root"):
            self.find({str(elsewhere): (1000, False)}, "apfsutil")

    def make_link_to_real_tool(self):
        real = self.root / "real/apfsutil-real"
        real.parent.mkdir(parents=True)
        real.write_text("fixture")
        real.chmod(0o755)
        link = self.bin / "apfsutil"
        link.symlink_to(real)
        return link, real

    def test_root_owned_symlink_to_root_owned_binary_is_accepted(self):
        link, real = self.make_link_to_real_tool()
        self.assertEqual(self.find(name="apfsutil"), str(real))

    def test_root_owned_symlinked_parent_is_accepted(self):
        alias = self.root / "usr/bin-alias"
        alias.symlink_to(self.bin, target_is_directory=True)
        self.assertEqual(self.find(directories=[str(alias)]), str(self.tool))
        self.assertEqual(cryptex.require_root_owned(str(alias / "apfs-fuse"), fake_lstat()), str(self.tool))

    def test_user_owned_symlink_is_refused(self):
        link, _ = self.make_link_to_real_tool()
        with self.assertRaisesRegex(ValueError, "owned by root"):
            self.find({str(link): (1000, False)}, "apfsutil")

    def test_symlink_to_user_writable_target_is_refused(self):
        link, real = self.make_link_to_real_tool()
        with self.assertRaisesRegex(ValueError, "not group- or world-writable"):
            self.find({str(real): (0, True)}, "apfsutil")
        with self.assertRaisesRegex(ValueError, "not group- or world-writable"):
            self.find({str(real.parent): (0, True)}, "apfsutil")

    def test_missing_tool_is_refused_and_not_searched_elsewhere(self):
        with self.assertRaisesRegex(ValueError, "was not found"):
            self.find(name="apfsutil")

    def test_non_executable_is_refused(self):
        self.tool.chmod(0o644)
        with self.assertRaisesRegex(ValueError, "not an executable"):
            self.find()

    def test_untrusted_first_location_does_not_fall_through(self):
        later = self.root / "usr/bin"
        later.mkdir(parents=True)
        (later / "apfs-fuse").write_text("fixture")
        (later / "apfs-fuse").chmod(0o755)
        with self.assertRaisesRegex(ValueError, "owned by root"):
            self.find({str(self.tool): (1000, False)}, directories=[str(self.bin), str(later)])


class MountRootTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(os.path.realpath(self.tmp.name))
        self.home = self.root / "home/me"
        self.home.mkdir(parents=True)

    def check(self, path, uid=UID, home=None):
        return cryptex.checked_root(str(path), uid, str(home or self.home))

    def test_directory_below_home_is_accepted_even_when_missing(self):
        self.assertEqual(self.check(self.home / "a/b/mounts"), self.home / "a/b/mounts")

    def test_symlinked_home_prefix_is_resolved_and_accepted(self):
        alias = self.root / "alias"
        alias.symlink_to(self.root / "home", target_is_directory=True)
        self.assertEqual(self.check(alias / "me/a/mounts", home=alias / "me"), self.home / "a/mounts")
        self.assertEqual(cryptex.resolve_home_prefix(str(alias / "me"), str(alias / "me")), str(self.home))

    def test_symlink_below_a_symlinked_home_prefix_is_still_refused(self):
        alias = self.root / "alias"
        alias.symlink_to(self.root / "home", target_is_directory=True)
        (self.home / "inner").symlink_to(self.root, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlink"):
            self.check(alias / "me/inner/mounts", home=alias / "me")

    def test_runtime_directory_is_accepted(self):
        self.assertEqual(self.check("/run/user/12345/darling/mounts", uid=12345), Path("/run/user/12345/darling/mounts"))

    def test_outside_home_is_refused(self):
        for path in (self.root / "elsewhere/mounts", "/etc/darling", "/run/user/999/mounts", self.home, self.home.parent / "other/mounts"):
            with self.assertRaisesRegex(ValueError, "below your home"):
                self.check(path)

    def test_dotdot_and_non_canonical_roots_are_refused(self):
        for path in (f"{self.home}/../other/mounts", f"{self.home}//mounts", f"{self.home}/./mounts", "relative/mounts"):
            with self.assertRaisesRegex(ValueError, "canonical"):
                self.check(path)

    def test_root_or_shallow_home_cannot_widen_the_anchor(self):
        with self.assertRaisesRegex(ValueError, "below your home"):
            self.check("/etc/mounts", home="/")
        with self.assertRaisesRegex(ValueError, "below your home"):
            self.check("/srv/mounts", home="/srv")

    def test_symlinked_component_is_refused(self):
        target = self.root / "target"
        target.mkdir()
        (self.home / "link").symlink_to(target, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlink"):
            self.check(self.home / "link/mounts")

    def test_final_symlink_is_refused(self):
        (self.home / "mounts").symlink_to(self.root, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlink"):
            self.check(self.home / "mounts")

    def test_component_owned_by_someone_else_is_refused(self):
        (self.home / "mounts").mkdir()
        with self.assertRaisesRegex(ValueError, "owned by the caller"):
            self.check(self.home / "mounts", uid=UID + 1)

    def test_file_component_is_refused(self):
        (self.home / "mounts").write_text("fixture")
        with self.assertRaisesRegex(ValueError, "owned by the caller"):
            self.check(self.home / "mounts/inner")

    def test_prepare_root_creates_directories_for_the_caller(self):
        root = cryptex.prepare_root(str(self.home / "a/mounts"), UID, os.getgid(), str(self.home))
        self.assertTrue(root.is_dir())
        self.assertEqual(stat.S_IMODE(root.stat().st_mode), 0o700)


class MountTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(os.path.realpath(self.tmp.name))
        self.home = self.root / "home"
        self.home.mkdir()
        self.mounts = self.home / "mounts"
        self.fuse = self.root / "user-fuse"
        self.fuse.write_text("fixture")
        self.fuse.chmod(0o700)
        self.options = SimpleNamespace(mount_root=str(self.mounts), apfs_fuse=str(self.fuse), device="")
        self.trusted = ("/usr/local/bin/apfs-fuse", "/usr/local/bin/apfsutil")
        self.calls, self.records, self.events = [], [], []
        self.gid = os.getgid()

    def emit(self, kind, **fields):
        self.events.append({"event": kind, **fields})

    def fake_run(self, argv):
        self.calls.append(argv)
        if argv[0] == "/usr/bin/lsblk":
            return json.dumps(lsblk(("/dev/synthetic2", [])))
        if argv[0] == "/usr/bin/findmnt":
            return json.dumps({"filesystems": self.records})
        if argv[0] == self.trusted[1]:
            return LIST
        self.assertIn(argv[0], (self.trusted[0], str(self.fuse)))
        self.records.append({"source": argv[-2], "target": argv[-1], "options": "rw"})
        if "allow_other" not in argv[2]:
            (Path(argv[-1]) / "root/System/Library/dyld").mkdir(parents=True)
        return ""

    def preboot(self, **overrides):
        arguments = dict(run_command=self.fake_run, emit_event=self.emit, check_device=lambda _: None, tools=lambda: self.trusted)
        arguments.update(overrides)
        return cryptex.mount_preboot(self.options, UID, self.gid, str(self.home), **arguments)

    def cryptex(self, **overrides):
        arguments = dict(run_command=self.fake_run, emit_event=self.emit, sleep=lambda _: None)
        arguments.update(overrides)
        return cryptex.mount_cryptex(self.options, UID, self.gid, str(self.home), **arguments)

    def test_preboot_step_builds_exact_argv_and_verifies(self):
        destination = self.preboot()
        fuse = [c for c in self.calls if c[0] == self.trusted[0]]
        self.assertEqual(fuse, [[self.trusted[0], "-o", f"allow_other,uid={UID},gid={self.gid}", "-v", "3", "/dev/synthetic2", str(destination)]])
        self.assertEqual(destination, self.mounts / "macos-preboot")
        self.assertEqual(self.events[-1], {"event": "mounted", "role": "preboot", "path": str(destination), "device": "/dev/synthetic2"})

    def test_privileged_step_ignores_caller_supplied_binaries(self):
        self.options.apfs_fuse = str(self.fuse)
        self.options.apfsutil = "/tmp/evil-util"
        self.preboot()
        self.assertFalse(any(c[0] in (str(self.fuse), "/tmp/evil-util") for c in self.calls))
        self.assertTrue(any(c[0] == self.trusted[0] for c in self.calls))

    def test_untrusted_system_tool_stops_before_any_mount(self):
        def tools():
            raise ValueError("untrusted")
        with self.assertRaisesRegex(ValueError, "untrusted"):
            self.preboot(tools=tools)
        self.assertFalse(any(c[0] == self.trusted[0] for c in self.calls))

    def test_preboot_step_refuses_mount_root_outside_home(self):
        self.options.mount_root = str(self.root / "outside/mounts")
        with self.assertRaisesRegex(ValueError, "below your home"):
            self.preboot()
        self.assertEqual(self.calls, [])
        self.assertFalse((self.root / "outside").exists())

    def test_preboot_step_refuses_symlinked_root_component(self):
        (self.home / "link").symlink_to(self.root, target_is_directory=True)
        self.options.mount_root = str(self.home / "link/mounts")
        with self.assertRaisesRegex(ValueError, "symlink"):
            self.preboot()
        self.assertEqual(self.calls, [])

    def test_preboot_mount_that_does_not_verify_fails(self):
        def run(argv):
            return "" if argv[0] == self.trusted[0] else self.fake_run(argv)
        with self.assertRaisesRegex(RuntimeError, "did not verify"):
            self.preboot(run_command=run)

    def test_cryptex_step_refuses_without_preboot(self):
        with self.assertRaisesRegex(ValueError, "Preboot volume is not mounted"):
            self.cryptex()

    def test_cryptex_step_refuses_as_root(self):
        with patch.object(cryptex.os, "geteuid", return_value=0), self.assertRaisesRegex(ValueError, "never as root"):
            self.cryptex()

    def test_cryptex_step_refuses_missing_apfs_fuse(self):
        preboot = self.mounts / "macos-preboot"
        make_dmg(preboot)
        self.records.append({"target": str(preboot)})
        self.options.apfs_fuse = str(self.root / "absent")
        with self.assertRaisesRegex(ValueError, "Missing executable"):
            self.cryptex()

    def test_cryptex_step_mounts_dmg_and_reports_dyld(self):
        preboot = self.mounts / "macos-preboot"
        image = make_dmg(preboot)
        self.records.append({"source": "/dev/synthetic2", "target": str(preboot)})
        dyld = self.cryptex()
        fuse = [c for c in self.calls if c[0] == str(self.fuse)]
        self.assertEqual(fuse, [[str(self.fuse), "-o", f"uid={UID},gid={self.gid}", "-v", "0", str(image.resolve()), str(self.mounts / "macos-cryptex-os")]])
        self.assertEqual(dyld, self.mounts / "macos-cryptex-os/root/System/Library/dyld")
        kinds = [e["event"] for e in self.events]
        self.assertLess(kinds.index("warning"), kinds.index("mounted"))
        self.assertEqual(kinds[-1], "dyld")
        self.assertIn("GB", self.events[kinds.index("warning")]["message"])

    def test_already_mounted_cryptex_is_not_mounted_twice(self):
        preboot, target = self.mounts / "macos-preboot", self.mounts / "macos-cryptex-os"
        (target / "root/System/Library/dyld").mkdir(parents=True)
        self.records += [{"target": str(preboot)}, {"target": str(target)}]
        self.cryptex()
        self.assertFalse(any(c[0] == str(self.fuse) for c in self.calls))

    def test_missing_dyld_directory_fails_after_waiting(self):
        preboot = self.mounts / "macos-preboot"
        make_dmg(preboot)
        self.records.append({"target": str(preboot)})
        sleeps = []
        def run(argv):
            if argv[0] == str(self.fuse):
                self.records.append({"target": argv[-1]})
                return ""
            return self.fake_run(argv)
        with self.assertRaisesRegex(RuntimeError, "did not appear"):
            self.cryptex(run_command=run, sleep=sleeps.append)
        self.assertEqual(len(sleeps), cryptex.DYLD_WAIT_SECONDS)


class MainTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = Path(os.path.realpath(self.tmp.name)) / "home"
        self.home.mkdir()
        self.args = ["--mount-root", str(self.home / "path with spaces"), "--apfs-fuse", "/tmp/fuse"]
        patcher = patch.object(cryptex.pwd, "getpwuid", return_value=SimpleNamespace(pw_dir=str(self.home), pw_gid=os.getgid()))
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_privileged_mode_requires_pkexec_identity(self):
        with patch.object(cryptex.os, "geteuid", return_value=1000), patch.object(cryptex.base, "emit") as emit:
            self.assertEqual(cryptex.main(["--privileged", *self.args]), 1)
            self.assertIn("pkexec", emit.call_args.kwargs["message"])

    def test_privileged_mode_uses_trusted_tools_not_arguments(self):
        with patch.object(cryptex, "caller_identity", return_value=(UID, os.getgid())), \
             patch.object(cryptex, "mount_preboot") as step:
            self.assertEqual(cryptex.main(["--privileged", *self.args]), 0)
            options = step.call_args.args[0]
            self.assertNotIn("tools", step.call_args.kwargs)
            self.assertEqual(options.apfs_fuse, "/tmp/fuse")
            self.assertEqual(step.call_args.args[3], str(self.home))

    def test_privileged_mode_with_untrusted_tool_fails_loudly(self):
        with patch.object(cryptex, "caller_identity", return_value=(UID, os.getgid())), \
             patch.object(cryptex, "system_tools", side_effect=ValueError("must be owned by root")), \
             patch.object(cryptex.base, "run", return_value="{}"), \
             patch.object(cryptex.base, "emit") as emit:
            self.assertEqual(cryptex.main(["--privileged", *self.args]), 1)
            self.assertIn("owned by root", emit.call_args.kwargs["message"])

    def authorize(self, code=0, extra=()):
        return (patch.object(cryptex.shutil, "which", return_value="/usr/bin/pkexec"),
                patch.object(cryptex, "require_root_owned", return_value="/trusted/python3"),
                patch.object(cryptex.base, "mounted_records", return_value=[]),
                patch.object(cryptex.subprocess, "call", return_value=code))

    def test_one_authorization_carries_no_binary_paths_then_cryptex_step(self):
        which, trust, records, call = self.authorize()
        with which, trust, records, call as call, patch.object(cryptex, "mount_cryptex") as step:
            self.assertEqual(cryptex.main([*self.args, "--device", "/dev/sda2"]), 0)
            self.assertEqual(call.call_count, 1)
            command = call.call_args.args[0]
            self.assertEqual(command[:3], ["/usr/bin/pkexec", "/trusted/python3", "-I"])
            self.assertEqual(command[command.index("--device") + 1], "/dev/sda2")
            self.assertIn("--privileged", command)
            self.assertIn(str(self.home / "path with spaces"), command)
            self.assertNotIn("--apfs-fuse", command)
            self.assertNotIn("/tmp/fuse", command)
            self.assertEqual(step.call_count, 1)

    def test_cancel_or_denial_stops_without_retry_or_cryptex_step(self):
        for code in (1, 126, 127):
            which, trust, records, call = self.authorize(code)
            with which, trust, records, call as call, patch.object(cryptex, "mount_cryptex") as step, patch.object(cryptex.base, "emit"):
                self.assertEqual(cryptex.main(self.args), code)
                self.assertEqual(call.call_count, 1)
                step.assert_not_called()

    def test_untrusted_interpreter_is_refused_before_pkexec(self):
        with patch.object(cryptex.shutil, "which", return_value="/usr/bin/pkexec"), \
             patch.object(cryptex.base, "mounted_records", return_value=[]), \
             patch.object(cryptex, "require_root_owned", side_effect=ValueError("must be owned by root")), \
             patch.object(cryptex.subprocess, "call") as call, patch.object(cryptex.base, "emit"):
            self.assertEqual(cryptex.main(self.args), 1)
            call.assert_not_called()

    def test_mount_root_outside_home_is_refused_before_pkexec(self):
        with patch.object(cryptex.base, "mounted_records", return_value=[]), \
             patch.object(cryptex.subprocess, "call") as call, patch.object(cryptex.base, "emit") as emit:
            self.assertEqual(cryptex.main(["--mount-root", "/etc/mounts"]), 1)
            call.assert_not_called()
            self.assertIn("below your home", emit.call_args.kwargs["message"])

    def test_mounted_preboot_skips_authorization(self):
        records = [{"target": str(self.home / "path with spaces/macos-preboot")}]
        with patch.object(cryptex.base, "mounted_records", return_value=records), \
             patch.object(cryptex.subprocess, "call") as call, patch.object(cryptex, "mount_cryptex") as step:
            self.assertEqual(cryptex.main(self.args), 0)
            call.assert_not_called()
            self.assertEqual(step.call_count, 1)

    def test_missing_pkexec_fails_loudly(self):
        with patch.object(cryptex.shutil, "which", return_value=None), patch.object(cryptex.base, "mounted_records", return_value=[]), \
             patch.object(cryptex.base, "emit") as emit:
            self.assertEqual(cryptex.main(self.args), 1)
            self.assertIn("pkexec", emit.call_args.kwargs["message"])


if __name__ == "__main__":
    unittest.main()
