#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Mount the macOS Preboot volume (one pkexec request), then its cryptex OS image as the user.

Everything is read-only (apfs-fuse cannot write). Reuses the helpers of mount-macos.py.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
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


def require_block_device(device):
    if not stat.S_ISBLK(os.stat(device).st_mode):
        raise ValueError(f"{device} is not a block device")


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
        base.validate_device(device)
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
    base.validate_device(device)
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


def mount_preboot(options, uid, gid, run_command=base.run, emit_event=base.emit, check_device=require_block_device):
    """Privileged step: discover the Preboot volume and mount it with allow_other for the caller."""
    root = base.prepare_root(options.mount_root, uid, gid)
    apfs_fuse, apfsutil = base.tool(options.apfs_fuse), base.tool(options.apfsutil)
    inventory = json.loads(run_command(["/usr/bin/lsblk", "--json", "--output", "PATH,FSTYPE,LABEL,MOUNTPOINTS"]))
    existing = base.mounted_records(run_command)
    device, index = find_preboot(inventory, run_command, apfsutil, existing, check_device, options.device or None)
    destination = root / PREBOOT_DIRECTORY
    base.prepare_destination(destination, existing, uid, gid)
    emit_event("progress", message=f"Mounting Preboot (volume {index} of {device}) read-only")
    run_command(preboot_arguments(apfs_fuse, uid, gid, index, device, destination))
    if not is_mounted(base.mounted_records(run_command), destination):
        raise RuntimeError("Preboot mount did not verify at the requested target")
    emit_event("mounted", role="preboot", path=str(destination), device=device)
    return destination


def mount_cryptex(options, uid, gid, run_command=base.run, emit_event=base.emit, sleep=time.sleep):
    """Unprivileged step: mount os.dmg from the mounted Preboot volume and report the dyld directory."""
    if os.geteuid() == 0:
        raise ValueError("The cryptex image is mounted as the user, never as root")
    root = base.prepare_root(options.mount_root, uid, gid)
    preboot = root / PREBOOT_DIRECTORY
    existing = base.mounted_records(run_command)
    if not is_mounted(existing, preboot):
        raise ValueError("The Preboot volume is not mounted")
    destination = root / CRYPTEX_DIRECTORY
    if not is_mounted(existing, destination):
        dmg = find_os_dmg(preboot)
        apfs_fuse = base.tool(options.apfs_fuse)
        base.prepare_destination(destination, existing, uid, gid)
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
    parser.add_argument("--apfs-fuse", default=shutil.which("apfs-fuse") or "")
    parser.add_argument("--apfsutil", default=shutil.which("apfsutil") or "")
    parser.add_argument("--device", default="", help="restrict the Preboot search to one APFS partition")
    parser.add_argument("--privileged", action="store_true", help=argparse.SUPPRESS)
    options = parser.parse_args(argv)
    try:
        if options.privileged:
            mount_preboot(options, *base.caller_identity())
            return 0
        uid, gid = os.getuid(), os.getgid()
        if uid == 0:
            raise ValueError("Run as the desktop user; the script requests root itself")
        preboot = Path(options.mount_root) / PREBOOT_DIRECTORY
        if not is_mounted(base.mounted_records(base.run), preboot):
            # Exactly one authorization request; no retries or privilege fallbacks.
            code = subprocess.call(base.authorization_command(options, __file__, ["--device", options.device] if options.device else []))
            if code in (126, 127):
                base.emit("error", message="Authorization cancelled or denied; stopped without retry")
            if code:
                return code
        mount_cryptex(options, uid, gid)
        return 0
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as error:
        base.emit("error", message=str(error))
        return 1


if __name__ == "__main__":
    sys.exit(main())
