#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Mount the macOS Preboot volume (one pkexec request), then its cryptex OS image as the user.

Everything is read-only (apfs-fuse cannot write). Reuses the read-only helpers of mount-macos.py.

Privileged step: root runs this script, so it must be installed root-owned (the launcher's
install step puts it and mount-macos.py in <libexecdir>/darling-launcher); a copy anywhere a
user can write is refused before any pkexec request and again at privileged entry. The helper
is compiled from its verified source, never imported, so a planted __pycache__ is not used.
The step takes no path from the caller: apfs-fuse and apfsutil come from fixed root-owned
locations and Preboot is mounted at /run/darling-launcher/<uid>/macos-preboot, a directory
chain owned by root that the caller cannot modify, so there is no check-then-use window on a
caller-controlled path. Not covered: the device nodes come from lsblk.
Unprivileged step: runs as the caller under the mount root (a canonical directory below the
home or /run/user/<uid>, validated by name), so it gains no privileges.
"""
import argparse
import errno
import json
import os
from pathlib import Path
import pwd
import re
import shutil
import stat
import struct
import subprocess
import sys
import time
import types

sys.dont_write_bytecode = True
HELPER = Path(__file__).resolve().with_name("mount-macos.py")
INSTALL_HINT = "install tools/mount-cryptex.py and tools/mount-macos.py root-owned into the same directory (sudo cmake --install <build directory>)"
base = None  # set by load_helper(); main() does it after the privileged trust checks


def emit(kind, **fields):
    print(json.dumps({"event": kind, **fields}), flush=True)


# Mirrored in src/mount.cpp (LauncherMount::cryptexMountDirectory).
PREBOOT_DIRECTORY = "macos-preboot"
CRYPTEX_DIRECTORY = "macos-cryptex-os"
DYLD_RELATIVE = Path("root/System/Library/dyld")
UUID = re.compile(r"[0-9A-Fa-f]{8}(-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}")
DYLD_WAIT_SECONDS = 30
TRUSTED_DIRECTORIES = ("/usr/local/bin", "/usr/bin")
RUNTIME_PARENT = "/run/darling-launcher"
ACL_NAME = "system.posix_acl_access"
UMOUNT = "/usr/bin/umount"


def validate_device(device):
    if not re.fullmatch(r"/dev/[A-Za-z0-9_/-]+", device) or ".." in device:
        raise ValueError("Invalid detected device path")


def require_block_device(device):
    if not stat.S_ISBLK(os.stat(device).st_mode):
        raise ValueError(f"{device} is not a block device")


def _require_root_chain(path, lstat):
    current = Path("/")
    for part in [""] + list(Path(path).parts[1:]):
        current = current / part
        info = lstat(current)
        # Linux reports 0777 for every symlink; the real target chain is checked separately.
        writable = not stat.S_ISLNK(info.st_mode) and info.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
        if info.st_uid != 0 or writable:
            raise ValueError(f"{current} must be owned by root and not group- or world-writable")


def require_root_owned(path, lstat=os.lstat):
    """Return the resolved path after checking the file and every ancestor, before and after symlinks."""
    if not os.path.isabs(path):
        raise ValueError(f"{path} must be an absolute path")
    resolved = os.path.realpath(path)
    _require_root_chain(os.path.normpath(path), lstat)
    _require_root_chain(resolved, lstat)
    return resolved


def read_trusted(path, lstat=os.lstat):
    """Return the text of a root-owned, not group/world-writable regular file under a root-owned chain."""
    resolved = require_root_owned(str(path), lstat)
    descriptor = os.open(resolved, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    with os.fdopen(descriptor, "r") as handle:
        if not stat.S_ISREG(os.fstat(handle.fileno()).st_mode):
            raise ValueError(f"{resolved} is not a regular file")
        return handle.read()


def load_helper(path=HELPER, trusted=False, lstat=os.lstat):
    """Compile the helper source directly: importing it would run a planted __pycache__ pyc."""
    text = read_trusted(path, lstat) if trusted else Path(path).read_text()
    module = types.ModuleType("mount_macos")
    module.__file__ = str(path)
    exec(compile(text, str(path), "exec"), module.__dict__)
    return module


def verify_installed(script=None, helper=HELPER, lstat=os.lstat):
    """Refuse to continue unless this script and its helper are where only root can change them."""
    for path in (script or Path(__file__).resolve(), helper):
        try:
            require_root_owned(str(path), lstat)
        except ValueError as error:
            raise ValueError(f"{error}; {INSTALL_HINT}") from None


def system_tool(name, directories=TRUSTED_DIRECTORIES, lstat=os.lstat):
    """Locate a helper only in fixed system directories; the caller's paths are never used."""
    for directory in directories:
        candidate = os.path.join(directory, name)
        if os.path.lexists(candidate):
            resolved = require_root_owned(candidate, lstat)
            if not stat.S_ISREG(os.stat(resolved).st_mode) or not os.access(resolved, os.X_OK):
                raise ValueError(f"{resolved} is not an executable file")
            return resolved
    raise ValueError(f"{name} was not found in {', '.join(directories)}; install it there as root")


def system_tools():
    return system_tool("apfs-fuse"), system_tool("apfsutil")


def normalise(path):
    """Collapse repeated slashes (POSIX normpath keeps a leading //), '.' and '..'; mirrored by QDir::cleanPath."""
    return os.path.normpath(re.sub(r"/{2,}", "/", path))


def resolve_home_prefix(mount_root, home):
    """Rewrite a leading home path to its resolved form so a symlinked home prefix (/home -> /var/home) is allowed.

    Mirrored by LauncherMount::resolveHomePrefix; tests/home_prefix_cases.json holds the shared cases."""
    raw, root = normalise(home), normalise(mount_root)
    if root == raw or root.startswith(raw.rstrip("/") + "/"):
        return os.path.realpath(raw) + root[len(raw):]
    return root


def checked_root(mount_root, uid, home, lstat=os.lstat):
    """Accept only a canonical path below home or /run/user/<uid> whose existing components below that are caller-owned directories.

    Symlinks in the home prefix itself are resolved first; none are allowed below it."""
    if not os.path.isabs(mount_root) or os.path.normpath(mount_root) != mount_root:
        raise ValueError("Choose a canonical absolute mount root")
    root = Path(resolve_home_prefix(mount_root, home))
    anchors = [Path(os.path.realpath(home)), Path(f"/run/user/{uid}")]
    anchor = next((a for a in anchors if len(a.parts) >= 3 and a in root.parents), None)
    if anchor is None:
        raise ValueError("The mount root must be a directory below your home or /run/user/<uid>")
    current = anchor
    for part in root.relative_to(anchor).parts:
        current = current / part
        try:
            info = lstat(current)
        except FileNotFoundError:
            break
        if stat.S_ISLNK(info.st_mode):
            raise ValueError(f"{current} is a symlink")
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != uid:
            raise ValueError(f"{current} is not a directory owned by the caller")
    return root


def prepare_root(mount_root, uid, home, lstat=os.lstat):
    root = checked_root(mount_root, uid, home, lstat)
    missing, ancestor = [], root
    while not os.path.lexists(ancestor):
        missing.append(ancestor)
        ancestor = ancestor.parent
    for directory in reversed(missing):
        os.mkdir(directory, 0o700)
    return checked_root(mount_root, uid, home, lstat)


def prepare_destination(destination, existing, uid):
    if destination.is_symlink() or destination.resolve() != destination:
        raise ValueError("Mount directory has symlink ancestors")
    destination.mkdir(exist_ok=True, mode=0o700)
    if destination.lstat().st_uid != uid:
        raise ValueError("Mount directory is not owned by the caller")
    if any(r.get("target") == str(destination) for r in existing) or any(destination.iterdir()):
        raise ValueError("Mount directory is mounted or not empty")


def preboot_path(uid, parent=RUNTIME_PARENT):
    return Path(parent) / str(uid) / PREBOOT_DIRECTORY


def caller_acl(uid):
    """system.posix_acl_access value: root rwx, only <uid> --x, nobody else (Linux POSIX ACL xattr version 2)."""
    undefined = 0xFFFFFFFF
    entries = ((0x01, 7, undefined),  # user::rwx
               (0x02, 1, uid),        # user:<uid>:--x
               (0x04, 0, undefined),  # group::---
               (0x10, 1, undefined),  # mask::--x
               (0x20, 0, undefined))  # other::---
    return struct.pack("<I", 2) + b"".join(struct.pack("<HHI", *entry) for entry in entries)


def _set_acl(path, name, value):
    os.setxattr(path, name, value, follow_symlinks=False)


def _get_acl(path, name):
    return os.getxattr(path, name, follow_symlinks=False)


def restrict_to_caller(directory, uid, setxattr=_set_acl, getxattr=_get_acl):
    """Make <directory> root:root 0700 plus an ACL entry that lets only the caller traverse it; fail closed."""
    os.chmod(directory, 0o700)  # before the ACL: chmod would reset the ACL mask
    acl = caller_acl(uid)
    try:
        setxattr(str(directory), ACL_NAME, acl)
        applied = getxattr(str(directory), ACL_NAME)
    except OSError as error:
        raise ValueError(f"Cannot restrict {directory} to your account with a POSIX ACL ({error.strerror or error}); "
                         "refusing to mount Preboot where other local users could read it") from None
    if applied != acl:
        raise ValueError(f"The POSIX ACL on {directory} was not applied as requested; refusing to mount Preboot")


def prepare_preboot_directory(uid, parent=RUNTIME_PARENT, lstat=os.lstat, setxattr=_set_acl, getxattr=_get_acl):
    """Create the root-owned Preboot mount point; refuse anything the caller could have influenced.

    /run/darling-launcher is 0755, /run/darling-launcher/<uid> is 0700 with an ACL for the caller only (the
    mounted volume uses allow_other and holds FileVault records, so other local users must not reach it)."""
    destination = preboot_path(uid, parent)
    if os.path.realpath(destination.parent.parent.parent) != str(destination.parent.parent.parent):
        raise ValueError(f"{destination.parent.parent.parent} must not involve symlinks")
    for directory in (destination.parent.parent, destination.parent, destination):
        if os.path.lexists(directory):
            if stat.S_ISLNK(os.lstat(directory).st_mode):
                raise ValueError(f"{directory} must not involve symlinks")
        else:
            os.mkdir(directory, 0o700 if directory == destination.parent else 0o755)
            os.chmod(directory, 0o700 if directory == destination.parent else 0o755)
        if directory == destination.parent:
            _require_root_chain(str(directory), lstat)
            restrict_to_caller(directory, uid, setxattr, getxattr)
    _require_root_chain(str(destination), lstat)
    if not stat.S_ISDIR(lstat(destination).st_mode):
        raise ValueError(f"{destination} is not a directory")
    return destination


def caller_identity():
    if os.geteuid() != 0 or not os.environ.get("PKEXEC_UID"):
        raise ValueError("Privileged mode requires pkexec and its caller identity")
    uid = int(os.environ["PKEXEC_UID"])
    if uid <= 0:
        raise ValueError("A non-root caller is required")
    return uid, pwd.getpwuid(uid).pw_gid


def authorization_command(options, lstat=os.lstat):
    """The request carries only the optional device; root chooses helper binaries and the mount point."""
    pkexec = shutil.which("pkexec")
    if not pkexec:
        raise ValueError("pkexec is unavailable")
    interpreter = require_root_owned(sys.executable, lstat)
    command = [pkexec, interpreter, "-I", str(Path(__file__).resolve()), "--privileged"]
    if options.unmount:
        return command + ["--unmount"]
    return command + (["--device", options.device] if options.device else [])


LSBLK = ["/usr/bin/lsblk", "--json", "--output", "PATH,TYPE,FSTYPE,LABEL,MOUNTPOINTS"]


def real_apfs_partitions(inventory):
    """APFS partitions of real disks only. Root parses these, and anyone can attach a loop device holding a
    crafted image, so loop (and any other non-disk) parents, and devices without a type, are never examined."""
    result = []
    for disk in inventory.get("blockdevices", []):
        if disk.get("type") != "disk" or disk.get("path", "").startswith("/dev/loop"):
            continue
        for partition in disk.get("children", []):
            if partition.get("type") == "part" and partition.get("fstype") == "apfs" and not partition.get("path", "").startswith("/dev/loop"):
                result.append(partition)
    return result


def find_preboot(inventory, run_command, apfsutil, existing, check_device, pinned=None):
    """Return (device, volume index) of the single Preboot volume; refuse zero or several."""
    candidates = real_apfs_partitions(inventory)
    if pinned and pinned not in [c.get("path") for c in candidates]:
        raise ValueError(f"{pinned} is not an APFS partition of a real disk (loop devices are never examined)")
    found = []
    for partition in candidates:
        device = partition.get("path", "")
        if pinned and device != pinned:
            continue
        validate_device(device)
        if any(partition.get("mountpoints", [])) or any(r.get("source") == device for r in existing):
            continue
        check_device(device)
        found += [(device, v["index"]) for v in base.volumes(run_command([apfsutil, device])) if v["name"] == "Preboot"]
    if not found:
        raise ValueError("No unmounted APFS partition of a real disk with a Preboot volume was found")
    if len(found) > 1:
        raise ValueError("Ambiguous Preboot volumes (" + ", ".join(f"{d} volume {i}" for d, i in found) + "); select one partition")
    return found[0]


def preboot_arguments(apfs_fuse, uid, gid, index, device, destination):
    if not isinstance(uid, int) or not isinstance(gid, int) or uid <= 0 or gid < 0:
        raise ValueError("A non-root caller identity is required")
    if not isinstance(index, int) or not 0 <= index < 100:
        raise ValueError("Invalid volume index")
    validate_device(device)
    return [apfs_fuse, "-o", f"allow_other,uid={uid},gid={gid}", "-v", str(index), device, str(destination)]


def cryptex_arguments(apfs_fuse, uid, gid, dmg, destination):
    if not isinstance(uid, int) or not isinstance(gid, int) or uid <= 0 or gid < 0:
        raise ValueError("A non-root caller identity is required")
    if not Path(dmg).is_absolute() or not Path(destination).is_absolute():
        raise ValueError("The image and mount directory must be absolute paths")
    return [apfs_fuse, "-o", f"uid={uid},gid={gid}", "-v", "0", str(dmg), str(destination)]


def find_os_dmg(preboot):
    """Return the single <preboot>/root/<UUID>/cryptex1/current/os.dmg that stays inside preboot."""
    listing = preboot / "root"
    if not listing.is_dir():
        raise ValueError(f"{listing} is missing; is the Preboot volume mounted?")
    anchor = preboot.resolve()
    found = []
    for entry in sorted(listing.iterdir()):
        if not UUID.fullmatch(entry.name):
            continue
        candidate = entry / "cryptex1/current/os.dmg"
        if candidate.is_file():
            resolved = candidate.resolve()
            if not resolved.is_relative_to(anchor):
                raise ValueError(f"{candidate} resolves outside the Preboot volume")
            found.append(resolved)
    if not found:
        raise ValueError(f"No <UUID>/cryptex1/current/os.dmg under {listing}")
    if len(found) > 1:
        raise ValueError("Ambiguous os.dmg images: " + ", ".join(str(p) for p in found))
    return found[0]


def is_mounted(records, target):
    return any(r.get("target") == str(target) for r in records)


def mount_state(path, records, stat_path=os.stat):
    """'absent', 'alive', or 'dead': listed in the mount table but its FUSE daemon is gone (ENOTCONN)."""
    if not is_mounted(records, path):
        return "absent"
    try:
        stat_path(path)
    except OSError as error:
        if error.errno == errno.ENOTCONN:
            return "dead"
        raise
    return "alive"


def release_preboot(destination, run_command, lstat=os.lstat):
    """Unmount root's own Preboot mount point, and nothing else."""
    parent = destination.parent
    if os.path.realpath(parent) != str(parent):
        raise ValueError(f"{parent} must not involve symlinks")
    _require_root_chain(str(parent), lstat)
    umount = require_root_owned(UMOUNT, lstat)
    try:
        run_command([umount, "--", str(destination)])
    except RuntimeError as error:
        raise RuntimeError(f"{error}; unmount the cryptex OS first if it is mounted") from None
    if is_mounted(base.mounted_records(run_command), destination):
        raise RuntimeError(f"{destination} is still mounted")


def unmount_preboot(uid, run_command=None, emit_event=emit, lstat=os.lstat, runtime_parent=RUNTIME_PARENT):
    """Privileged step for --unmount: only the known /run mount point of the caller."""
    run_command = run_command or base.run
    destination = preboot_path(uid, runtime_parent)
    if mount_state(destination, base.mounted_records(run_command)) == "absent":
        raise ValueError("Preboot is not mounted")
    release_preboot(destination, run_command, lstat)
    emit_event("unmounted", role="preboot", path=str(destination))


def mount_preboot(options, uid, gid, run_command=None, emit_event=emit, check_device=require_block_device,
                  tools=None, lstat=os.lstat, runtime_parent=RUNTIME_PARENT, setxattr=_set_acl, getxattr=_get_acl):
    """Privileged step. Only options.device is used; binaries and the mount point are chosen here."""
    run_command = run_command or base.run
    apfs_fuse, apfsutil = (tools or system_tools)()
    destination = preboot_path(uid, runtime_parent)
    existing = base.mounted_records(run_command)
    state = mount_state(destination, existing)
    if state == "alive":
        raise ValueError(f"Preboot is already mounted at {destination}")
    if state == "dead":
        emit_event("warning", message="The previous Preboot mount lost its apfs-fuse daemon; unmounting it first")
        release_preboot(destination, run_command, lstat)
        existing = base.mounted_records(run_command)
    inventory = json.loads(run_command(LSBLK))
    device, index = find_preboot(inventory, run_command, apfsutil, existing, check_device, options.device or None)
    destination = prepare_preboot_directory(uid, runtime_parent, lstat, setxattr, getxattr)
    if any(destination.iterdir()):
        raise ValueError(f"{destination} is not empty")
    emit_event("progress", message=f"Mounting Preboot (volume {index} of {device}) read-only")
    run_command(preboot_arguments(apfs_fuse, uid, gid, index, device, destination))
    if not is_mounted(base.mounted_records(run_command), destination):
        raise RuntimeError("Preboot mount did not verify at the requested target")
    emit_event("mounted", role="preboot", path=str(destination), device=device)
    return destination


def mount_cryptex(options, uid, gid, home, run_command=None, emit_event=emit, sleep=time.sleep,
                  runtime_parent=RUNTIME_PARENT):
    """Unprivileged step: mount os.dmg from the mounted Preboot volume and report the dyld directory."""
    if os.geteuid() == 0:
        raise ValueError("The cryptex image is mounted as the user, never as root")
    run_command = run_command or base.run
    root = prepare_root(options.mount_root, uid, home)
    preboot = preboot_path(uid, runtime_parent)
    existing = base.mounted_records(run_command)
    state = mount_state(preboot, existing)
    if state == "absent":
        raise ValueError("The Preboot volume is not mounted")
    if state == "dead":
        raise ValueError("The Preboot mount is not responding (its apfs-fuse daemon exited); run this action again to remount it")
    destination = root / CRYPTEX_DIRECTORY
    if mount_state(destination, existing) == "dead":
        raise ValueError(f"The cryptex mount at {destination} is not responding (its apfs-fuse daemon exited); unmount it with fusermount3 -u and retry")
    if not is_mounted(existing, destination):
        dmg = find_os_dmg(preboot)
        apfs_fuse = base.tool(options.apfs_fuse)
        prepare_destination(destination, existing, uid)
        emit_event("warning", message="apfs-fuse can use several GB of memory while reading the cryptex image; unmount it if the system slows down")
        emit_event("progress", message=f"Mounting {dmg} read-only")
        run_command(cryptex_arguments(apfs_fuse, uid, gid, dmg, destination))
        if not is_mounted(base.mounted_records(run_command), destination):
            raise RuntimeError("Cryptex mount did not verify at the requested target")
    emit_event("mounted", role="cryptex-os", path=str(destination))
    dyld = destination / DYLD_RELATIVE
    for _ in range(DYLD_WAIT_SECONDS):
        if dyld.is_dir():
            emit_event("dyld", path=str(dyld))
            return dyld
        sleep(1)
    raise RuntimeError(f"Mounted, but {dyld} did not appear within {DYLD_WAIT_SECONDS} seconds")


if __name__ != "__main__":
    base = load_helper()


def main(argv=None):
    global base
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mount-root", default="", help="where the unprivileged image mount goes")
    parser.add_argument("--apfs-fuse", default=shutil.which("apfs-fuse") or "", help="used for the unprivileged image mount only")
    parser.add_argument("--device", default="", help="restrict the Preboot search to one APFS partition")
    parser.add_argument("--unmount", action="store_true", help="unmount Preboot (one pkexec request); unmount the cryptex OS first")
    parser.add_argument("--privileged", action="store_true", help=argparse.SUPPRESS)
    options = parser.parse_args(argv)
    try:
        if options.privileged:
            verify_installed()
            base = load_helper(trusted=True)
            uid, gid = caller_identity()
            if options.unmount:
                unmount_preboot(uid)
            else:
                mount_preboot(options, uid, gid)
            return 0
        uid, gid = os.getuid(), os.getgid()
        if uid == 0:
            raise ValueError("Run as the desktop user; the script requests root itself")
        base = base or load_helper()
        state = mount_state(preboot_path(uid), base.mounted_records(base.run))
        if options.unmount and state == "absent":
            emit("progress", message="Preboot is not mounted")
            return 0
        if not options.unmount:
            home = pwd.getpwuid(uid).pw_dir
            options.mount_root = str(checked_root(options.mount_root, uid, home))
        if options.unmount or state != "alive":
            if state == "dead":
                emit("warning", message="The Preboot mount lost its apfs-fuse daemon and will be replaced")
            verify_installed()
            # Exactly one authorization request; no retries or privilege fallbacks.
            code = subprocess.call(authorization_command(options))
            if code in (126, 127):
                emit("error", message="Authorization cancelled or denied; stopped without retry")
            if code or options.unmount:
                return code
        mount_cryptex(options, uid, gid, home)
        return 0
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as error:
        emit("error", message=str(error))
        return 1


if __name__ == "__main__":
    sys.exit(main())
