#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Mount the macOS Preboot volume (one pkexec request), then its cryptex OS image as the user.

Everything is read-only (apfs-fuse cannot write). Reuses the read-only helpers of mount-macos.py.
The privileged step never runs binaries or uses a mount root named by the unprivileged caller:
apfs-fuse and apfsutil come from fixed root-owned locations and the mount root must be a
caller-owned, symlink-free directory under the caller's home or /run/user/<uid>.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import pwd
import re
import shutil
import stat
import subprocess
import sys
import time

_spec = importlib.util.spec_from_file_location("mount_macos", Path(__file__).resolve().with_name("mount-macos.py"))
base = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(base)

# Mirrored in src/mount.cpp (LauncherMount::cryptexMountDirectory).
PREBOOT_DIRECTORY = "macos-preboot"
CRYPTEX_DIRECTORY = "macos-cryptex-os"
DYLD_RELATIVE = Path("root/System/Library/dyld")
UUID = re.compile(r"[0-9A-Fa-f]{8}(-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}")
DYLD_WAIT_SECONDS = 30
TRUSTED_DIRECTORIES = ("/usr/local/bin", "/usr/bin")


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


def resolve_home_prefix(mount_root, home):
    """Rewrite a leading home path to its resolved form so a symlinked home prefix (/home -> /var/home) is allowed."""
    raw = os.path.normpath(home)
    if mount_root == raw or mount_root.startswith(raw.rstrip("/") + "/"):
        return os.path.realpath(home) + mount_root[len(raw):]
    return mount_root


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


def prepare_root(mount_root, uid, gid, home, lstat=os.lstat):
    root = checked_root(mount_root, uid, home, lstat)
    missing, ancestor = [], root
    while not os.path.lexists(ancestor):
        missing.append(ancestor)
        ancestor = ancestor.parent
    for directory in reversed(missing):
        os.mkdir(directory, 0o700)
        os.chown(directory, uid, gid, follow_symlinks=False)
    return checked_root(mount_root, uid, home, lstat)


def prepare_destination(destination, existing, uid, gid):
    if destination.is_symlink() or destination.resolve() != destination:
        raise ValueError("Mount directory has symlink ancestors")
    new_directory = not destination.exists()
    destination.mkdir(exist_ok=True, mode=0o700)
    if new_directory:
        os.chown(destination, uid, gid, follow_symlinks=False)
    elif destination.lstat().st_uid != uid:
        raise ValueError("Mount directory is not owned by the caller")
    if any(r.get("target") == str(destination) for r in existing) or any(destination.iterdir()):
        raise ValueError("Mount directory is mounted or not empty")


def caller_identity():
    if os.geteuid() != 0 or not os.environ.get("PKEXEC_UID"):
        raise ValueError("Privileged mode requires pkexec and its caller identity")
    uid = int(os.environ["PKEXEC_UID"])
    if uid <= 0:
        raise ValueError("A non-root caller is required")
    return uid, pwd.getpwuid(uid).pw_gid


def authorization_command(options, lstat=os.lstat):
    """The request carries only the mount root and optional device; root chooses the helper binaries."""
    pkexec = shutil.which("pkexec")
    if not pkexec:
        raise ValueError("pkexec is unavailable")
    interpreter = require_root_owned(sys.executable, lstat)
    command = [pkexec, interpreter, "-I", str(Path(__file__).resolve()), "--privileged", "--mount-root", options.mount_root]
    return command + (["--device", options.device] if options.device else [])


def find_preboot(inventory, run_command, apfsutil, existing, check_device, pinned=None):
    """Return (device, volume index) of the single Preboot volume; refuse zero or several."""
    candidates = base.partitions(inventory)
    if pinned and pinned not in [c.get("path") for c in candidates]:
        raise ValueError(f"{pinned} is not a detected APFS partition")
    found = []
    for partition in candidates:
        device = partition.get("path", "")
        if partition.get("fstype") != "apfs" or (pinned and device != pinned):
            continue
        validate_device(device)
        if any(partition.get("mountpoints", [])) or any(r.get("source") == device for r in existing):
            continue
        check_device(device)
        found += [(device, v["index"]) for v in base.volumes(run_command([apfsutil, device])) if v["name"] == "Preboot"]
    if not found:
        raise ValueError("No unmounted APFS partition with a Preboot volume was found")
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


def mount_preboot(options, uid, gid, home, run_command=base.run, emit_event=base.emit,
                  check_device=require_block_device, tools=None, lstat=os.lstat):
    """Privileged step. options.apfs_fuse is ignored: tools() supplies the root-trusted binaries."""
    root = prepare_root(options.mount_root, uid, gid, home, lstat)
    apfs_fuse, apfsutil = (tools or system_tools)()
    inventory = json.loads(run_command(["/usr/bin/lsblk", "--json", "--output", "PATH,FSTYPE,LABEL,MOUNTPOINTS"]))
    existing = base.mounted_records(run_command)
    device, index = find_preboot(inventory, run_command, apfsutil, existing, check_device, options.device or None)
    destination = root / PREBOOT_DIRECTORY
    prepare_destination(destination, existing, uid, gid)
    checked_root(options.mount_root, uid, home, lstat)
    emit_event("progress", message=f"Mounting Preboot (volume {index} of {device}) read-only")
    run_command(preboot_arguments(apfs_fuse, uid, gid, index, device, destination))
    if not is_mounted(base.mounted_records(run_command), destination):
        raise RuntimeError("Preboot mount did not verify at the requested target")
    emit_event("mounted", role="preboot", path=str(destination), device=device)
    return destination


def mount_cryptex(options, uid, gid, home, run_command=base.run, emit_event=base.emit, sleep=time.sleep):
    """Unprivileged step: mount os.dmg from the mounted Preboot volume and report the dyld directory."""
    if os.geteuid() == 0:
        raise ValueError("The cryptex image is mounted as the user, never as root")
    root = prepare_root(options.mount_root, uid, gid, home)
    preboot = root / PREBOOT_DIRECTORY
    existing = base.mounted_records(run_command)
    if not is_mounted(existing, preboot):
        raise ValueError("The Preboot volume is not mounted")
    destination = root / CRYPTEX_DIRECTORY
    if not is_mounted(existing, destination):
        dmg = find_os_dmg(preboot)
        apfs_fuse = base.tool(options.apfs_fuse)
        prepare_destination(destination, existing, uid, gid)
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


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mount-root", required=True)
    parser.add_argument("--apfs-fuse", default=shutil.which("apfs-fuse") or "", help="used for the unprivileged image mount only")
    parser.add_argument("--device", default="", help="restrict the Preboot search to one APFS partition")
    parser.add_argument("--privileged", action="store_true", help=argparse.SUPPRESS)
    options = parser.parse_args(argv)
    try:
        if options.privileged:
            uid, gid = caller_identity()
            mount_preboot(options, uid, gid, pwd.getpwuid(uid).pw_dir)
            return 0
        uid, gid = os.getuid(), os.getgid()
        if uid == 0:
            raise ValueError("Run as the desktop user; the script requests root itself")
        home = pwd.getpwuid(uid).pw_dir
        root = checked_root(options.mount_root, uid, home)
        options.mount_root = str(root)
        if not is_mounted(base.mounted_records(base.run), root / PREBOOT_DIRECTORY):
            # Exactly one authorization request; no retries or privilege fallbacks.
            code = subprocess.call(authorization_command(options))
            if code in (126, 127):
                base.emit("error", message="Authorization cancelled or denied; stopped without retry")
            if code:
                return code
        mount_cryptex(options, uid, gid, home)
        return 0
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as error:
        base.emit("error", message=str(error))
        return 1


if __name__ == "__main__":
    sys.exit(main())
