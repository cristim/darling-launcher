#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
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
    """One real disk with the given APFS partitions."""
    return {"blockdevices": [{"path": "/dev/sda", "type": "disk", "children": [
        {"path": d, "type": "part", "fstype": "apfs", "mountpoints": m} for d, m in devices]}]}


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
        with self.assertRaisesRegex(ValueError, "No APFS partition"):
            self.find(lsblk(("/dev/sda2", [])), {"/dev/sda2": OTHER})
        with self.assertRaisesRegex(ValueError, "No APFS partition"):
            self.find({"blockdevices": []}, {})

    def test_mounted_partitions_are_skipped(self):
        with self.assertRaisesRegex(ValueError, "No APFS partition"):
            self.find(lsblk(("/dev/sda2", ["/mnt"])), {"/dev/sda2": LIST})
        with self.assertRaisesRegex(ValueError, "No APFS partition"):
            self.find(lsblk(("/dev/sda2", [])), {"/dev/sda2": LIST}, existing=[{"source": "/dev/sda2", "target": "/x"}])

    def test_other_volumes_mounted_by_apfs_fuse_do_not_hide_the_partition(self):
        inventory = lsblk(("/dev/sda2", ["/home/me/mounts/sda2-volume-1"]))
        for fstype in ("fuse", "fuse.apfs-fuse"):
            for source in ("/dev/sda2", "apfs-fuse"):
                records = [{"source": source, "target": "/home/me/mounts/sda2-volume-1", "fstype": fstype}]
                self.assertEqual(self.find(inventory, {"/dev/sda2": LIST}, existing=records), ("/dev/sda2", 3), (fstype, source))
        self.assertEqual(self.find(lsblk(("/dev/sda2", [])), {"/dev/sda2": LIST}, existing=[{"source": "/dev/sda2", "target": "/x", "fstype": "fuse"}]), ("/dev/sda2", 3))

    def test_partitions_held_by_another_driver_are_refused(self):
        cases = {
            "kernel apfs": (lsblk(("/dev/sda2", ["/mnt/mac"])), [{"source": "/dev/sda2", "target": "/mnt/mac", "fstype": "apfs"}]),
            "ext4": (lsblk(("/dev/sda2", ["/mnt/data"])), [{"source": "/dev/sda2", "target": "/mnt/data", "fstype": "ext4"}]),
            "fuseblk": (lsblk(("/dev/sda2", ["/mnt/x"])), [{"source": "/dev/sda2", "target": "/mnt/x", "fstype": "fuseblk"}]),
            "record without a type": (lsblk(("/dev/sda2", [])), [{"source": "/dev/sda2", "target": "/mnt/x"}]),
            "lsblk mount point the table cannot explain": (lsblk(("/dev/sda2", ["/mnt/ghost"])), []),
            "fuse volume plus a kernel mount": (lsblk(("/dev/sda2", ["/a", "/b"])), [{"source": "apfs-fuse", "target": "/a", "fstype": "fuse"}, {"source": "/dev/sda2", "target": "/b", "fstype": "apfs"}]),
        }
        for name, (inventory, records) in cases.items():
            with self.assertRaisesRegex(ValueError, "No APFS partition", msg=name):
                self.find(inventory, {"/dev/sda2": LIST}, existing=records)

    def test_ambiguity_is_refused_unless_pinned(self):
        inventory, listings = lsblk(("/dev/sda2", []), ("/dev/sdb2", [])), {"/dev/sda2": LIST, "/dev/sdb2": LIST}
        with self.assertRaisesRegex(ValueError, "Ambiguous"):
            self.find(inventory, listings)
        self.assertEqual(self.find(inventory, listings, pinned="/dev/sdb2"), ("/dev/sdb2", 3))
        with self.assertRaisesRegex(ValueError, "not an APFS partition of a real disk"):
            self.find(inventory, listings, pinned="/dev/sdc2")

    def test_two_preboot_volumes_on_one_partition_are_ambiguous(self):
        with self.assertRaisesRegex(ValueError, "Ambiguous"):
            self.find(lsblk(("/dev/sda2", [])), {"/dev/sda2": LIST + LIST.replace("Volume 3", "Volume 4")})

    def test_invalid_device_aborts(self):
        with self.assertRaises(ValueError):
            self.find(lsblk(("/dev/sda2;x", [])), {})

    def test_loop_devices_are_never_examined_even_when_pinned(self):
        crafted = {"blockdevices": [{"path": "/dev/loop0", "type": "loop", "children": [
            {"path": "/dev/loop0p1", "type": "part", "fstype": "apfs", "mountpoints": []}]}]}
        listings = {"/dev/loop0p1": LIST}
        probed = []
        def run(argv):
            probed.append(argv)
            return LIST
        for pinned in (None, "/dev/loop0p1"):
            with self.assertRaises(ValueError):
                cryptex.find_preboot(crafted, run, "/util", [], lambda _: None, pinned)
        self.assertEqual(probed, [])

    def test_only_partitions_of_real_disks_are_examined(self):
        def tree(path, kind, parent_kind, fstype="apfs"):
            return {"blockdevices": [{"path": "/dev/x", "type": parent_kind, "children": [{"path": path, "type": kind, "fstype": fstype, "mountpoints": []}]}]}
        probed = []
        def run(argv):
            probed.append(argv)
            return LIST
        rejected = [tree("/dev/mapper/crypt", "crypt", "disk"), tree("/dev/sda2", "part", "lvm"), tree("/dev/sda2", "part", "loop"),
                    tree("/dev/sda2", "part", None), tree("/dev/loop1p1", "part", "disk"),
                    {"blockdevices": [{"path": "/dev/sda", "type": "disk", "fstype": "apfs", "mountpoints": []}]},
                    {"blockdevices": [{"path": "/dev/loop2", "type": "disk", "children": [{"path": "/dev/loop2p1", "type": "part", "fstype": "apfs"}]}]}]
        for inventory in rejected:
            with self.assertRaises(ValueError):
                cryptex.find_preboot(inventory, run, "/util", [], lambda _: None)
        self.assertEqual(probed, [])
        self.assertEqual(cryptex.find_preboot(tree("/dev/sda2", "part", "disk"), run, "/util", [], lambda _: None), ("/dev/sda2", 3))

    def test_inventory_without_type_information_is_refused(self):
        untyped = {"blockdevices": [{"path": "/dev/sda", "children": [{"path": "/dev/sda2", "fstype": "apfs"}]}]}
        with self.assertRaisesRegex(ValueError, "No APFS partition"):
            cryptex.find_preboot(untyped, lambda argv: LIST, "/util", [], lambda _: None)

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

    def test_os_dmg_discovery_works_without_path_is_relative_to(self):
        import pathlib
        with patch.object(pathlib.PurePath, "is_relative_to", side_effect=AttributeError("Python 3.8"), create=True), \
             tempfile.TemporaryDirectory() as directory:
            preboot = Path(directory) / "preboot"
            image = make_dmg(preboot)
            self.assertEqual(cryptex.find_os_dmg(preboot), image.resolve())
            outside = Path(directory) / "outside"
            (outside / "cryptex1/current").mkdir(parents=True)
            (outside / "cryptex1/current/os.dmg").write_text("fixture")
            (preboot / "root" / UUID_B).symlink_to(outside, target_is_directory=True)
            with self.assertRaisesRegex(ValueError, "outside"):
                cryptex.find_os_dmg(preboot)

    def test_containment_check_matches_is_relative_to(self):
        for path, anchor, inside in (("/a/b/c", "/a/b", True), ("/a/b", "/a/b", True), ("/a/bc", "/a/b", False), ("/a", "/a/b", False), ("/x/y", "/a/b", False)):
            self.assertEqual(cryptex.is_inside(path, anchor), inside, (path, anchor))
            self.assertEqual(Path(path).is_relative_to(anchor), inside)
        self.assertFalse(cryptex.is_inside("relative/x", "/a/b"))

    def test_old_python_is_refused_with_a_clear_message(self):
        with self.assertRaisesRegex(ValueError, r"Python 3\.8 or newer"):
            cryptex.require_python((3, 7, 9))
        cryptex.require_python((3, 8, 0))
        cryptex.require_python((4, 0))

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


class CallerIdentityTest(unittest.TestCase):
    def identity(self, value, euid=0):
        environment = {} if value is None else {"PKEXEC_UID": value}
        with patch.dict(cryptex.os.environ, environment, clear=True), patch.object(cryptex.os, "geteuid", return_value=euid), \
             patch.object(cryptex.pwd, "getpwuid", return_value=SimpleNamespace(pw_gid=4321)):
            return cryptex.caller_identity()

    def test_pkexec_caller_is_accepted(self):
        self.assertEqual(self.identity("1000"), (1000, 4321))

    def test_root_negative_and_garbage_callers_are_refused(self):
        for value in ("0", "-1", "-1000", "abc", "", None):
            with self.assertRaises(ValueError, msg=repr(value)):
                self.identity(value)

    def test_root_caller_message_names_the_reason(self):
        with self.assertRaisesRegex(ValueError, "non-root caller"):
            self.identity("0")

    def test_not_running_as_root_is_refused(self):
        with self.assertRaisesRegex(ValueError, "pkexec and its caller identity"):
            self.identity("1000", euid=1000)


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

    def test_shared_cases_match_the_cpp_implementation_inputs(self):
        real = self.root / "real"
        (real / "me").mkdir(parents=True)
        (self.root / "alias").symlink_to(real, target_is_directory=True)
        cases = json.loads((Path(__file__).resolve().parent / "home_prefix_cases.json").read_text())
        self.assertGreater(len(cases), 5)
        for case in cases:
            fill = lambda text: text.replace("@ALIAS@", str(self.root / "alias")).replace("@REAL@", str(real))
            self.assertEqual(cryptex.resolve_home_prefix(fill(case["root"]), fill(case["home"])), fill(case["expect"]), case["name"])

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

    def test_prepare_root_creates_private_directories(self):
        root = cryptex.prepare_root(str(self.home / "a/mounts"), UID, str(self.home))
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
        self.runtime = self.root / "run/darling-launcher"
        self.runtime.parent.mkdir()
        self.preboot_dir = self.runtime / str(UID) / "macos-preboot"
        self.fuse = self.root / "user-fuse"
        self.fuse.write_text("fixture")
        self.fuse.chmod(0o700)
        self.options = SimpleNamespace(mount_root=str(self.mounts), apfs_fuse=str(self.fuse), device="")
        self.root_owned = fake_lstat()
        self.acls = {}
        self.umount_error = False
        self.lsblk_mountpoints = []
        self.trusted = ("/usr/local/bin/apfs-fuse", "/usr/local/bin/apfsutil")
        self.calls, self.records, self.events = [], [], []
        self.gid = os.getgid()

    def emit(self, kind, **fields):
        self.events.append({"event": kind, **fields})

    def fake_run(self, argv):
        self.calls.append(argv)
        if argv[0] == "/usr/bin/lsblk":
            return json.dumps(lsblk(("/dev/synthetic2", self.lsblk_mountpoints)))
        if argv[0] == "/usr/bin/findmnt":
            return json.dumps({"filesystems": self.records})
        if argv[0] == self.trusted[1]:
            return LIST
        if argv[0].endswith("/umount"):
            if self.umount_error:
                raise RuntimeError("umount failed (32): target is busy")
            self.records[:] = [r for r in self.records if r.get("target") != argv[-1]]
            return ""
        self.assertIn(argv[0], (self.trusted[0], str(self.fuse)))
        self.records.append({"source": argv[-2], "target": argv[-1], "options": "rw"})
        if "allow_other" not in argv[2]:
            (Path(argv[-1]) / "root/System/Library/dyld").mkdir(parents=True)
        return ""

    def set_acl(self, path, name, value):
        self.assertEqual(name, "system.posix_acl_access")
        self.acls[path] = value

    def get_acl(self, path, name):
        return self.acls[path]

    def preboot(self, **overrides):
        arguments = dict(run_command=self.fake_run, emit_event=self.emit, check_device=lambda _: None, tools=lambda: self.trusted,
                         lstat=self.root_owned, runtime_parent=str(self.runtime),
                         setxattr=self.set_acl, getxattr=self.get_acl)
        arguments.update(overrides)
        return cryptex.mount_preboot(self.options, UID, self.gid, **arguments)

    def cryptex(self, **overrides):
        arguments = dict(run_command=self.fake_run, emit_event=self.emit, sleep=lambda _: None, runtime_parent=str(self.runtime))
        arguments.update(overrides)
        return cryptex.mount_cryptex(self.options, UID, self.gid, str(self.home), **arguments)

    def test_preboot_step_builds_exact_argv_and_verifies(self):
        destination = self.preboot()
        self.assertIn(["/usr/bin/lsblk", "--json", "--output", "PATH,TYPE,FSTYPE,LABEL,MOUNTPOINTS"], self.calls)
        fuse = [c for c in self.calls if c[0] == self.trusted[0]]
        self.assertEqual(fuse, [[self.trusted[0], "-o", f"allow_other,uid={UID},gid={self.gid}", "-v", "3", "/dev/synthetic2", str(destination)]])
        self.assertEqual(destination, self.preboot_dir)
        self.assertTrue(destination.is_dir())
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

    def test_privileged_step_never_touches_the_caller_mount_root(self):
        victim = self.root / "victim/mounts"
        self.options.mount_root = str(victim)
        destination = self.preboot()
        self.assertFalse((self.root / "victim").exists())
        self.assertEqual(destination, self.preboot_dir)
        self.assertNotIn(str(victim), " ".join(" ".join(c) for c in self.calls))

    def test_preboot_directory_chain_is_not_caller_modifiable(self):
        for overrides, message in (({str(self.runtime / str(UID)): (UID, False)}, "owned by root"),
                                   ({str(self.runtime): (0, True)}, "not group- or world-writable"),
                                   ({str(self.preboot_dir): (0, True)}, "not group- or world-writable")):
            self.calls.clear()
            with self.assertRaisesRegex(ValueError, message):
                self.preboot(lstat=fake_lstat(overrides))
            self.assertFalse(any(c[0] == self.trusted[0] for c in self.calls))

    def test_symlink_planted_in_the_runtime_chain_is_refused_and_its_target_untouched(self):
        elsewhere = self.root / "elsewhere"
        elsewhere.mkdir()
        self.runtime.mkdir(parents=True)
        (self.runtime / str(UID)).symlink_to(elsewhere, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlinks"):
            self.preboot()
        self.assertEqual(list(elsewhere.iterdir()), [])
        self.assertFalse(any(c[0] == self.trusted[0] for c in self.calls))

    def test_planted_symlink_at_the_mount_point_is_refused(self):
        elsewhere = self.root / "elsewhere"
        elsewhere.mkdir()
        self.preboot_dir.parent.mkdir(parents=True)
        self.preboot_dir.symlink_to(elsewhere, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlinks"):
            self.preboot()
        self.assertFalse(any(c[0] == self.trusted[0] for c in self.calls))

    def test_symlinked_run_directory_itself_is_refused_and_its_target_untouched(self):
        real = self.root / "real-run"
        real.mkdir()
        (self.root / "link-run").symlink_to(real, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlinks"):
            self.preboot(runtime_parent=str(self.root / "link-run/darling-launcher"))
        self.assertEqual(list(real.iterdir()), [])
        self.assertFalse(any(c[0] == self.trusted[0] for c in self.calls))

    def test_non_empty_unmounted_preboot_directory_is_refused(self):
        self.preboot_dir.mkdir(parents=True)
        (self.preboot_dir / "leftover").write_text("x")
        with self.assertRaisesRegex(ValueError, "not empty"):
            self.preboot()
        self.assertFalse(any(c[0] == self.trusted[0] for c in self.calls))

    def test_caller_acl_bytes_are_exactly_root_rwx_caller_x_nobody_else(self):
        undefined = 0xFFFFFFFF
        value = cryptex.caller_acl(1234)
        self.assertEqual(struct.unpack("<I", value[:4])[0], 2)
        entries = [struct.unpack("<HHI", value[4 + 8 * i:12 + 8 * i]) for i in range(5)]
        self.assertEqual(entries, [(0x01, 7, undefined), (0x02, 1, 1234), (0x04, 0, undefined), (0x10, 1, undefined), (0x20, 0, undefined)])
        self.assertEqual(len(value), 4 + 5 * 8)

    def test_uid_directory_is_private_with_an_acl_for_the_caller_only(self):
        destination = self.preboot()
        uid_dir = destination.parent
        self.assertEqual(self.acls, {str(uid_dir): cryptex.caller_acl(UID)})
        self.assertEqual(stat.S_IMODE(uid_dir.stat().st_mode), 0o700)
        self.assertEqual(stat.S_IMODE(self.runtime.stat().st_mode), 0o755)

    def test_acl_is_applied_before_the_mount_point_exists_and_again_on_reuse(self):
        def check_first(path, name, value):
            self.assertFalse(self.preboot_dir.exists())
            self.set_acl(path, name, value)
        args = (UID, str(self.runtime), self.root_owned)
        cryptex.prepare_preboot_directory(*args, check_first, self.get_acl)
        calls = []
        cryptex.prepare_preboot_directory(*args, lambda *a: calls.append(a) or self.set_acl(*a), self.get_acl)
        self.assertEqual(len(calls), 1)

    def test_missing_acl_support_fails_closed_before_any_mount(self):
        import errno
        def unsupported(path, name, value):
            raise OSError(errno.ENOTSUP, "Operation not supported")
        with self.assertRaisesRegex(ValueError, "POSIX ACL.*refusing to mount"):
            self.preboot(setxattr=unsupported)
        self.assertFalse(any(c[0] == self.trusted[0] for c in self.calls))
        self.assertFalse(self.preboot_dir.exists())

    def test_acl_that_reads_back_differently_fails_closed(self):
        with self.assertRaisesRegex(ValueError, "not applied as requested"):
            self.preboot(getxattr=lambda path, name: b"\x02\x00\x00\x00")
        self.assertFalse(any(c[0] == self.trusted[0] for c in self.calls))

    def umount_path(self):
        return os.path.realpath(cryptex.UMOUNT)

    def test_alive_preboot_mount_is_reused_not_mounted_over(self):
        self.preboot_dir.mkdir(parents=True)
        self.records.append({"source": "/dev/synthetic2", "target": str(self.preboot_dir), "fstype": "fuse"})
        with patch.object(cryptex, "mount_state", return_value="alive"):
            destination = self.preboot()
        self.assertEqual(destination, self.preboot_dir)
        self.assertEqual(self.events, [{"event": "mounted", "role": "preboot", "path": str(self.preboot_dir), "device": "/dev/synthetic2"}])
        self.assertFalse(any(c[0] in (self.trusted[0], self.trusted[1], self.umount_path()) for c in self.calls))

    def reused_mount(self):
        self.preboot_dir.mkdir(parents=True)
        os.chmod(self.preboot_dir.parent, 0o755)  # left by an earlier build
        self.records.append({"source": "/dev/synthetic2", "target": str(self.preboot_dir), "fstype": "fuse"})

    def test_reused_mount_gets_its_parent_directory_tightened(self):
        self.reused_mount()
        with patch.object(cryptex, "mount_state", return_value="alive"):
            self.preboot()
        self.assertEqual(stat.S_IMODE(self.preboot_dir.parent.stat().st_mode), 0o700)
        self.assertEqual(self.acls, {str(self.preboot_dir.parent): cryptex.caller_acl(UID)})
        self.assertEqual(self.events[-1]["event"], "mounted")

    def test_reused_mount_whose_parent_cannot_be_restricted_is_refused(self):
        import errno
        self.reused_mount()
        def unsupported(path, name, value):
            raise OSError(errno.ENOTSUP, "Operation not supported")
        with patch.object(cryptex, "mount_state", return_value="alive"), self.assertRaisesRegex(ValueError, "POSIX ACL.*refusing"):
            self.preboot(setxattr=unsupported)
        self.assertEqual(self.events, [])

    def test_reused_mount_under_an_untrusted_parent_is_refused(self):
        self.reused_mount()
        with patch.object(cryptex, "mount_state", return_value="alive"), self.assertRaisesRegex(ValueError, "owned by root"):
            self.preboot(lstat=fake_lstat({str(self.preboot_dir.parent): (1000, False)}))
        self.assertEqual(self.acls, {})
        self.assertEqual(self.events, [])

    def test_batch_mounted_sibling_volume_does_not_block_the_preboot_mount(self):
        sibling = self.home / "mounts/synthetic2-volume-1"
        self.records.append({"source": "/dev/synthetic2", "target": str(sibling), "fstype": "fuse"})
        self.lsblk_mountpoints = [str(sibling)]
        destination = self.preboot()
        self.assertTrue(any(c[0] == self.trusted[0] for c in self.calls))
        self.assertEqual(self.events[-1]["path"], str(destination))

    def test_kernel_mounted_partition_blocks_the_preboot_mount(self):
        self.records.append({"source": "/dev/synthetic2", "target": "/mnt/mac", "fstype": "apfs"})
        self.lsblk_mountpoints = ["/mnt/mac"]
        with self.assertRaisesRegex(ValueError, "No APFS partition"):
            self.preboot()
        self.assertFalse(any(c[0] == self.trusted[0] for c in self.calls))

    def test_dead_preboot_mount_is_unmounted_before_the_device_is_searched(self):
        self.preboot_dir.mkdir(parents=True)
        self.records.append({"source": "/dev/synthetic2", "target": str(self.preboot_dir)})
        with patch.object(cryptex, "mount_state", return_value="dead"):
            self.preboot()
        order = [c[0] for c in self.calls if c[0] in (self.umount_path(), self.trusted[1], self.trusted[0])]
        self.assertEqual(order, [self.umount_path(), self.trusted[1], self.trusted[0]])
        self.assertIn([self.umount_path(), "--", str(self.preboot_dir)], self.calls)
        self.assertEqual(self.events[0]["event"], "warning")

    def test_unmount_uses_only_the_callers_known_mount_point(self):
        self.preboot_dir.mkdir(parents=True)
        self.records.append({"target": str(self.preboot_dir)})
        with patch.object(cryptex, "mount_state", return_value="alive"):
            cryptex.unmount_preboot(UID, self.fake_run, self.emit, self.root_owned, str(self.runtime))
        self.assertEqual([c for c in self.calls if c[0] == self.umount_path()], [[self.umount_path(), "--", str(self.preboot_dir)]])
        self.assertEqual(self.events[-1], {"event": "unmounted", "role": "preboot", "path": str(self.preboot_dir)})

    def test_unmount_of_a_dead_mount_works_too(self):
        self.preboot_dir.mkdir(parents=True)
        self.records.append({"target": str(self.preboot_dir)})
        with patch.object(cryptex, "mount_state", return_value="dead"):
            cryptex.unmount_preboot(UID, self.fake_run, self.emit, self.root_owned, str(self.runtime))
        self.assertEqual(self.records, [])

    def test_unmount_that_leaves_the_mount_listed_is_an_error(self):
        self.preboot_dir.mkdir(parents=True)
        self.records.append({"target": str(self.preboot_dir)})
        def run(argv):
            return "" if argv[0] == self.umount_path() else self.fake_run(argv)  # umount "succeeds" but the mount stays
        with patch.object(cryptex, "mount_state", return_value="alive"), self.assertRaisesRegex(RuntimeError, "still mounted"):
            cryptex.unmount_preboot(UID, run, self.emit, self.root_owned, str(self.runtime))
        self.assertFalse(any(e["event"] == "unmounted" for e in self.events))

    def test_unmount_refusals(self):
        with self.assertRaisesRegex(ValueError, "not mounted"):
            cryptex.unmount_preboot(UID, self.fake_run, self.emit, self.root_owned, str(self.runtime))
        self.preboot_dir.mkdir(parents=True)
        self.records.append({"target": str(self.preboot_dir)})
        self.umount_error = True
        with patch.object(cryptex, "mount_state", return_value="alive"), self.assertRaisesRegex(RuntimeError, "unmount the cryptex OS first"):
            cryptex.unmount_preboot(UID, self.fake_run, self.emit, self.root_owned, str(self.runtime))
        self.umount_error = False
        with patch.object(cryptex, "mount_state", return_value="alive"), self.assertRaisesRegex(ValueError, "owned by root"):
            cryptex.unmount_preboot(UID, self.fake_run, self.emit, fake_lstat({str(self.preboot_dir.parent): (1000, False)}), str(self.runtime))
        self.assertEqual(len(self.records), 1)

    def test_unmount_refuses_a_symlinked_parent_and_runs_nothing(self):
        elsewhere = self.root / "elsewhere"
        (elsewhere / "macos-preboot").mkdir(parents=True)
        self.runtime.mkdir(parents=True)
        (self.runtime / str(UID)).symlink_to(elsewhere, target_is_directory=True)
        self.records.append({"target": str(self.preboot_dir)})
        with patch.object(cryptex, "mount_state", return_value="alive"), self.assertRaisesRegex(ValueError, "symlinks"):
            cryptex.unmount_preboot(UID, self.fake_run, self.emit, self.root_owned, str(self.runtime))
        self.assertFalse(any(c[0] == self.umount_path() for c in self.calls))

    def test_cryptex_step_reports_a_dead_preboot_or_cryptex_mount(self):
        preboot = self.preboot_dir
        make_dmg(preboot)
        self.records.append({"target": str(preboot)})
        with patch.object(cryptex, "mount_state", return_value="dead"), self.assertRaisesRegex(ValueError, "not responding.*remount"):
            self.cryptex()
        states = iter(["alive", "dead"])
        with patch.object(cryptex, "mount_state", side_effect=lambda *a: next(states)), self.assertRaisesRegex(ValueError, "cryptex mount.*not responding"):
            self.cryptex()
        self.assertFalse(any(c[0] == str(self.fuse) for c in self.calls))

    def test_untrusted_tool_creates_no_directories(self):
        def tools():
            raise ValueError("untrusted")
        with self.assertRaises(ValueError):
            self.preboot(tools=tools)
        self.assertFalse(self.runtime.exists())

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
        preboot = self.preboot_dir
        make_dmg(preboot)
        self.records.append({"target": str(preboot)})
        self.options.apfs_fuse = str(self.root / "absent")
        with self.assertRaisesRegex(ValueError, "Missing executable"):
            self.cryptex()

    def test_cryptex_step_mounts_dmg_and_reports_dyld(self):
        preboot = self.preboot_dir
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
        preboot, target = self.preboot_dir, self.mounts / "macos-cryptex-os"
        (target / "root/System/Library/dyld").mkdir(parents=True)
        preboot.mkdir(parents=True)
        self.records += [{"target": str(preboot)}, {"target": str(target)}]
        self.cryptex()
        self.assertFalse(any(c[0] == str(self.fuse) for c in self.calls))

    def test_missing_dyld_directory_fails_after_waiting(self):
        preboot = self.preboot_dir
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


class MountStateTest(unittest.TestCase):
    def test_states(self):
        import errno
        records = [{"target": "/m"}]
        self.assertEqual(cryptex.mount_state("/other", records, lambda p: 1 / 0), "absent")
        self.assertEqual(cryptex.mount_state("/m", records, lambda p: None), "alive")
        def dead(path):
            raise OSError(errno.ENOTCONN, "Transport endpoint is not connected")
        self.assertEqual(cryptex.mount_state("/m", records, dead), "dead")

    def test_other_errors_are_not_mistaken_for_a_dead_mount(self):
        import errno
        def denied(path):
            raise OSError(errno.EACCES, "Permission denied")
        with self.assertRaises(OSError):
            cryptex.mount_state("/m", [{"target": "/m"}], denied)


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
        self.verify = patch.object(cryptex, "verify_installed")
        self.verify.start()
        self.addCleanup(self.verify.stop)
        self.loader = patch.object(cryptex, "load_helper", return_value=cryptex.base)
        self.loader.start()
        self.addCleanup(self.loader.stop)
        keep = patch.object(cryptex, "base", cryptex.base)
        keep.start()
        self.addCleanup(keep.stop)

    def test_privileged_mode_requires_pkexec_identity(self):
        with patch.object(cryptex.os, "geteuid", return_value=1000), patch.object(cryptex, "emit") as emit:
            self.assertEqual(cryptex.main(["--privileged", *self.args]), 1)
            self.assertIn("pkexec", emit.call_args.kwargs["message"])

    def test_privileged_mode_uses_trusted_tools_not_arguments(self):
        with patch.object(cryptex, "caller_identity", return_value=(UID, os.getgid())), \
             patch.object(cryptex, "mount_preboot") as step:
            self.assertEqual(cryptex.main(["--privileged", *self.args]), 0)
            options = step.call_args.args[0]
            self.assertNotIn("tools", step.call_args.kwargs)
            self.assertEqual(options.apfs_fuse, "/tmp/fuse")
            self.assertEqual(step.call_args.args[1:], (UID, os.getgid()))

    def test_privileged_mode_with_untrusted_tool_fails_loudly(self):
        with patch.object(cryptex, "caller_identity", return_value=(UID, os.getgid())), \
             patch.object(cryptex, "system_tools", side_effect=ValueError("must be owned by root")), \
             patch.object(cryptex.base, "run", return_value="{}"), \
             patch.object(cryptex, "emit") as emit:
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
            self.assertNotIn("--mount-root", command)
            self.assertNotIn(str(self.home / "path with spaces"), command)
            self.assertNotIn("--apfs-fuse", command)
            self.assertNotIn("/tmp/fuse", command)
            self.assertEqual(step.call_count, 1)

    def test_cancel_or_denial_stops_without_retry_or_cryptex_step(self):
        for code in (1, 126, 127):
            which, trust, records, call = self.authorize(code)
            with which, trust, records, call as call, patch.object(cryptex, "mount_cryptex") as step, patch.object(cryptex, "emit"):
                self.assertEqual(cryptex.main(self.args), code)
                self.assertEqual(call.call_count, 1)
                step.assert_not_called()

    def test_untrusted_interpreter_is_refused_before_pkexec(self):
        with patch.object(cryptex.shutil, "which", return_value="/usr/bin/pkexec"), \
             patch.object(cryptex.base, "mounted_records", return_value=[]), \
             patch.object(cryptex, "require_root_owned", side_effect=ValueError("must be owned by root")), \
             patch.object(cryptex.subprocess, "call") as call, patch.object(cryptex, "emit"):
            self.assertEqual(cryptex.main(self.args), 1)
            call.assert_not_called()

    def test_mount_root_outside_home_is_refused_before_pkexec(self):
        with patch.object(cryptex.base, "mounted_records", return_value=[]), \
             patch.object(cryptex.subprocess, "call") as call, patch.object(cryptex, "emit") as emit:
            self.assertEqual(cryptex.main(["--mount-root", "/etc/mounts"]), 1)
            call.assert_not_called()
            self.assertIn("below your home", emit.call_args.kwargs["message"])

    def test_mounted_preboot_skips_authorization(self):
        records = [{"target": f"/run/darling-launcher/{UID}/macos-preboot"}]
        with patch.object(cryptex.base, "mounted_records", return_value=records), patch.object(cryptex, "mount_state", return_value="alive"), \
             patch.object(cryptex.subprocess, "call") as call, patch.object(cryptex, "mount_cryptex") as step:
            self.assertEqual(cryptex.main(self.args), 0)
            call.assert_not_called()
            self.assertEqual(step.call_count, 1)

    def authorized_main(self, argv, state, code=0):
        with patch.object(cryptex.shutil, "which", return_value="/usr/bin/pkexec"), \
             patch.object(cryptex, "require_root_owned", return_value="/trusted/python3"), \
             patch.object(cryptex.base, "mounted_records", return_value=[]), patch.object(cryptex, "mount_state", return_value=state), \
             patch.object(cryptex.subprocess, "call", return_value=code) as call, patch.object(cryptex, "mount_cryptex") as step, \
             patch.object(cryptex, "emit") as emit:
            result = cryptex.main(argv)
        return result, call, step, emit

    def test_dead_preboot_mount_requests_one_authorization_to_replace_it(self):
        result, call, step, emit = self.authorized_main(self.args, "dead")
        self.assertEqual(result, 0)
        self.assertEqual(call.call_count, 1)
        self.assertNotIn("--unmount", call.call_args.args[0])
        self.assertEqual(step.call_count, 1)
        self.assertEqual(emit.call_args_list[0].args[0], "warning")

    def test_unmount_request_is_one_authorization_without_mount_root_or_cryptex_step(self):
        result, call, step, emit = self.authorized_main(["--unmount"], "alive")
        self.assertEqual(result, 0)
        self.assertEqual(call.call_count, 1)
        command = call.call_args.args[0]
        self.assertEqual(command[-2:], ["--privileged", "--unmount"])
        self.assertNotIn("--mount-root", command)
        step.assert_not_called()

    def test_unmount_when_nothing_is_mounted_asks_for_no_authorization(self):
        result, call, step, emit = self.authorized_main(["--unmount"], "absent")
        self.assertEqual(result, 0)
        call.assert_not_called()

    def test_unmount_failure_or_denial_is_reported_without_retry(self):
        for code in (1, 126, 127):
            result, call, step, emit = self.authorized_main(["--unmount"], "alive", code)
            self.assertEqual(result, code)
            self.assertEqual(call.call_count, 1)

    def test_privileged_unmount_never_mounts(self):
        with patch.object(cryptex, "caller_identity", return_value=(UID, os.getgid())), \
             patch.object(cryptex, "unmount_preboot") as unmount, patch.object(cryptex, "mount_preboot") as mount:
            self.assertEqual(cryptex.main(["--privileged", "--unmount"]), 0)
        unmount.assert_called_once_with(UID)
        mount.assert_not_called()

    def test_missing_pkexec_fails_loudly(self):
        with patch.object(cryptex.shutil, "which", return_value=None), patch.object(cryptex.base, "mounted_records", return_value=[]), \
             patch.object(cryptex, "emit") as emit:
            self.assertEqual(cryptex.main(self.args), 1)
            self.assertIn("pkexec", emit.call_args.kwargs["message"])


class InstallTest(unittest.TestCase):
    """Root must never run, or load, anything the user can rewrite."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(os.path.realpath(self.tmp.name)) / "libexec/darling-launcher"
        self.dir.mkdir(parents=True)
        self.script, self.helper = self.dir / "mount-cryptex.py", self.dir / "mount-macos.py"
        self.script.write_text("# script\n")
        self.helper.write_text("MARK = 'good'\n")

    def verify(self, overrides=None):
        cryptex.verify_installed(self.script, self.helper, fake_lstat(overrides))

    def test_root_owned_install_is_accepted(self):
        self.verify()

    def test_user_owned_script_or_helper_is_refused_with_the_install_hint(self):
        for path in (self.script, self.helper):
            with self.assertRaisesRegex(ValueError, "owned by root.*cmake --install"):
                self.verify({str(path): (1000, False)})

    def test_group_writable_install_directory_is_refused(self):
        for path in (self.dir, self.dir.parent):
            with self.assertRaisesRegex(ValueError, "not group- or world-writable"):
                self.verify({str(path): (0, True)})

    def test_writable_script_file_is_refused(self):
        with self.assertRaisesRegex(ValueError, "not group- or world-writable"):
            self.verify({str(self.helper): (0, True)})

    def test_trusted_read_checks_ownership_before_returning_source(self):
        self.assertEqual(cryptex.read_trusted(self.helper, fake_lstat()), "MARK = 'good'\n")
        with self.assertRaisesRegex(ValueError, "owned by root"):
            cryptex.read_trusted(self.helper, fake_lstat({str(self.helper): (1000, False)}))

    def test_trusted_load_refuses_a_user_owned_helper_without_running_it(self):
        self.helper.write_text("raise SystemExit('executed')\n")
        with self.assertRaisesRegex(ValueError, "owned by root"):
            cryptex.load_helper(self.helper, trusted=True, lstat=fake_lstat({str(self.helper): (1000, False)}))

    def test_planted_pycache_is_not_executed(self):
        import importlib.util as util
        import py_compile
        evil = Path(self.tmp.name) / "evil.py"
        evil.write_text("MARK = 'evil'\n")
        cache = Path(util.cache_from_source(str(self.helper)))
        cache.parent.mkdir()
        py_compile.compile(str(evil), cfile=str(cache), doraise=True, invalidation_mode=py_compile.PycInvalidationMode.UNCHECKED_HASH)
        spec = util.spec_from_file_location("imported", self.helper)
        imported = util.module_from_spec(spec)
        spec.loader.exec_module(imported)
        self.assertEqual(imported.MARK, "evil")  # the attack is real for a normal import
        self.assertEqual(cryptex.load_helper(self.helper, trusted=True, lstat=fake_lstat()).MARK, "good")

    def test_bytecode_is_never_written(self):
        self.assertTrue(cryptex.sys.dont_write_bytecode)
        cryptex.load_helper(self.helper, trusted=True, lstat=fake_lstat())
        self.assertFalse((self.dir / "__pycache__").exists())

    def test_privileged_entry_verifies_before_loading_anything(self):
        order = []
        loads = []
        with patch.object(cryptex, "verify_installed", side_effect=lambda *a, **k: order.append("verify")), \
             patch.object(cryptex, "load_helper", side_effect=lambda *a, **k: order.append("load") or loads.append(k) or cryptex.base), \
             patch.object(cryptex, "caller_identity", return_value=(UID, os.getgid())), \
             patch.object(cryptex, "mount_preboot"), patch.object(cryptex, "base", cryptex.base):
            self.assertEqual(cryptex.main(["--privileged"]), 0)
        self.assertEqual(order, ["verify", "load"])
        self.assertEqual(loads, [{"trusted": True}])

    def test_privileged_entry_stops_on_untrusted_install(self):
        with patch.object(cryptex, "verify_installed", side_effect=ValueError("not root-owned")), \
             patch.object(cryptex, "load_helper") as load, patch.object(cryptex, "mount_preboot") as step, \
             patch.object(cryptex, "emit") as emit:
            self.assertEqual(cryptex.main(["--privileged"]), 1)
            load.assert_not_called()
            step.assert_not_called()
            self.assertIn("not root-owned", emit.call_args.kwargs["message"])

    def test_unprivileged_run_refuses_before_pkexec_when_not_installed(self):
        home = Path(self.tmp.name) / "home"
        home.mkdir()
        with patch.object(cryptex.pwd, "getpwuid", return_value=SimpleNamespace(pw_dir=str(home), pw_gid=os.getgid())), \
             patch.object(cryptex.base, "mounted_records", return_value=[]), \
             patch.object(cryptex, "verify_installed", side_effect=ValueError("not root-owned")), \
             patch.object(cryptex.subprocess, "call") as call, patch.object(cryptex, "emit"):
            self.assertEqual(cryptex.main(["--mount-root", str(home / "mounts")]), 1)
            call.assert_not_called()


if __name__ == "__main__":
    unittest.main()
