#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Enumerate and mount detected macOS volumes read-only with one pkexec request."""
import argparse
import json
import os
from pathlib import Path
import pwd
import re
import shutil
import stat
import subprocess
import sys


def emit(kind, **fields):
    print(json.dumps({"event": kind, **fields}), flush=True)


def partitions(document):
    result = []
    def collect(items):
        for item in items:
            if item.get("fstype") in ("apfs", "hfs", "hfsplus"):
                result.append(item)
            collect(item.get("children", []))
    collect(document.get("blockdevices", []))
    return result


def volumes(text):
    result, current = [], None
    for line in text.splitlines():
        if line.startswith("Volume "):
            current = None
            match = re.fullmatch(r"Volume (\d+) [0-9A-Fa-f-]{36}\s*", line)
            if match and 0 <= int(match[1]) < 100 and not any(v["index"] == int(match[1]) for v in result):
                current = {"index": int(match[1]), "name": "", "encrypted": True}
                result.append(current)
        elif current is not None:
            if line.startswith("Name:"):
                current["name"] = line[5:].strip()
            elif line.startswith("FileVault:"):
                current["encrypted"] = line[10:].strip() != "No"
    return result


def run(arguments):
    completed = subprocess.run(arguments, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if completed.returncode:
        raise RuntimeError(f"{Path(arguments[0]).name} failed ({completed.returncode}): {completed.stdout.strip()}")
    return completed.stdout


def tool(path):
    candidate = Path(path)
    if not candidate.is_absolute() or not candidate.is_file() or not os.access(candidate, os.X_OK):
        raise ValueError(f"Missing executable: {path}")
    return str(candidate.resolve())


def mounted_records(run_command):
    document = json.loads(run_command(["/usr/bin/findmnt", "--json", "--list", "--output", "SOURCE,TARGET,OPTIONS"]))
    return document.get("filesystems", [])


def mount_batch(options, uid, gid, run_command=run, emit_event=emit, check_device=None):
    root = Path(options.mount_root)
    if not root.is_absolute() or root == Path("/") or root.resolve() != root:
        raise ValueError("Choose an absolute mount root without symlink ancestors")
    missing = []
    ancestor = root
    while not ancestor.exists():
        missing.append(ancestor)
        ancestor = ancestor.parent
    root.mkdir(parents=True, exist_ok=True)
    if os.geteuid() == 0:
        for directory in reversed(missing):
            os.chown(directory, uid, gid)
            directory.chmod(0o700)
    if root.is_symlink() or not root.is_dir():
        raise ValueError("Invalid mount root")
    inventory = partitions(json.loads(run_command(["/usr/bin/lsblk", "--json", "--output", "PATH,FSTYPE,LABEL,MOUNTPOINTS"])))
    existing = mounted_records(run_command)
    owned = []
    for partition in inventory:
        device = partition.get("path", "")
        try:
            if not re.fullmatch(r"/dev/[A-Za-z0-9_/-]+", device) or ".." in device:
                raise ValueError("Invalid detected device path")
            if any(partition.get("mountpoints", [])) or any(r.get("source") == device for r in existing):
                emit_event("skip", device=device, reason="Already mounted")
                continue
            if check_device:
                check_device(device)
            elif not stat.S_ISBLK(os.stat(device).st_mode):
                raise ValueError("Detected path is not a block device")
            if partition["fstype"] == "apfs":
                utility, driver = tool(options.apfsutil), tool(options.apfs_fuse)
                emit_event("progress", message=f"Enumerating {device}")
                entries = volumes(run_command([utility, device]))
                if not entries:
                    emit_event("skip", device=device, reason="No readable APFS volumes found")
            else:
                driver = "/usr/bin/mount"
                entries = [{"index": -1, "name": partition.get("label") or device, "encrypted": False}]
            for volume in entries:
                if volume["encrypted"]:
                    emit_event("skip", device=device, volume=volume["index"], reason="Encrypted or encryption state unknown; unlock separately")
                    continue
                destination = root / f"{Path(device).name}-volume-{volume['index']}"
                try:
                    if destination.is_symlink() or destination.resolve() != destination:
                        raise ValueError("Mount directory has symlink ancestors")
                    new_directory = not destination.exists()
                    destination.mkdir(exist_ok=True)
                    if new_directory and os.geteuid() == 0:
                        os.chown(destination, uid, gid)
                        destination.chmod(0o700)
                    if any(r.get("target") == str(destination) for r in existing) or any(destination.iterdir()):
                        raise ValueError("Mount directory is mounted or not empty")
                    flags = f"ro,nodev,nosuid,noexec,uid={uid},gid={gid}"
                    emit_event("progress", message=f"Mounting {device} volume {volume['index']} ({volume['name']}) read-only")
                    arguments = [driver, "-v", str(volume["index"]), "-o", flags + ",allow_other", device, str(destination)] if partition["fstype"] == "apfs" else [driver, "-i", "-t", partition["fstype"], "-o", flags, "--", device, str(destination)]
                    run_command(arguments)
                    records = mounted_records(run_command)
                    verified = next((r for r in records if r.get("source") == device and r.get("target") == str(destination)), None)
                    if not verified:
                        raise RuntimeError("Mount did not verify as the requested device and target")
                    if not {"ro", "nodev", "nosuid", "noexec"}.issubset(set(verified.get("options", "").split(","))):
                        run_command(["/usr/bin/umount", "--", str(destination)])
                        raise RuntimeError("Mount did not verify as read-only with requested restrictions; unmounted")
                    existing = records
                    owned.append(str(destination))
                    emit_event("mounted", path=str(destination), device=device)
                except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as error:
                    emit_event("error", device=device, volume=volume["index"], message=str(error))
        except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as error:
            emit_event("error", device=device, message=str(error))
    sources = []
    for record in mounted_records(run_command):
        path = Path(record.get("target", "/"))
        if path != Path("/") and (path / "System/Library").is_dir() and ((path / "Applications").is_dir() or (path / "System/Applications").is_dir()):
            sources.append(str(path))
    emit_event("complete", mounts=owned, sources=sorted(set(sources)))
    return owned


def authorization_command(options):
    pkexec = shutil.which("pkexec")
    if not pkexec:
        raise ValueError("pkexec is unavailable")
    return [pkexec, str(Path(sys.executable).resolve()), "-I", str(Path(__file__).resolve()), "--privileged",
            "--mount-root", options.mount_root, "--apfs-fuse", options.apfs_fuse, "--apfsutil", options.apfsutil]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mount-root", required=True)
    parser.add_argument("--apfs-fuse", default=shutil.which("apfs-fuse") or "")
    parser.add_argument("--apfsutil", default=shutil.which("apfsutil") or "")
    parser.add_argument("--privileged", action="store_true", help=argparse.SUPPRESS)
    options = parser.parse_args(argv)
    try:
        if not options.privileged:
            # Exactly one authorization request; no retries or privilege fallbacks.
            code = subprocess.call(authorization_command(options))
            if code in (126, 127):
                emit("error", message="Authorization cancelled or denied; stopped without retry")
            return code
        if os.geteuid() != 0 or not os.environ.get("PKEXEC_UID"):
            raise ValueError("Privileged mode requires pkexec and its caller identity")
        uid = int(os.environ["PKEXEC_UID"])
        if uid <= 0:
            raise ValueError("A non-root caller is required")
        mount_batch(options, uid, pwd.getpwuid(uid).pw_gid)
        return 0
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as error:
        emit("error", message=str(error))
        return 1


if __name__ == "__main__":
    sys.exit(main())
