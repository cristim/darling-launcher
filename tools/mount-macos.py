#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Enumerate and mount detected macOS volumes read-only with one pkexec request."""
import argparse
import errno
import json
import os
from pathlib import Path
import pwd
import re
import stat
import struct
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
    completed = subprocess.run(arguments, text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    if completed.returncode:
        # Tool output read as root may describe things the caller cannot read; report only the status.
        raise RuntimeError(f"{Path(arguments[0]).name} failed with exit status {completed.returncode}")
    return completed.stdout


def describe(error):
    # OSError text carries the path it failed on; the caller gets only the reason.
    return (error.strerror or type(error).__name__) if isinstance(error, OSError) else str(error)


PKEXEC = "/usr/bin/pkexec"
PYTHON = "/usr/bin/python3"
INSTALL_HINT = "install it root-owned and not group/other writable, e.g. under /usr/local/libexec/darling-launcher"
# Root runs these tools on attacker-influenced devices, so only fixed install locations are accepted.
APFS_TOOLS = {
    "apfsutil": ("/usr/bin/apfsutil", "/usr/local/bin/apfsutil"),
    "apfs-fuse": ("/usr/bin/apfs-fuse", "/usr/local/bin/apfs-fuse"),
}


def trusted(path):
    """Resolve path and require every component of the result to be root-owned and not group/other writable."""
    if not path or not os.path.isabs(path):
        raise ValueError(f"Not an absolute path: {path!r}")
    resolved = os.path.realpath(path)
    current = "/"
    for part in Path(resolved).parts:
        current = os.path.join(current, part)
        status = os.lstat(current)
        if stat.S_ISLNK(status.st_mode) or status.st_uid != 0 or status.st_mode & 0o022:
            raise ValueError(f"Refusing to run {path} as root: {current} is not root-owned or is writable by others; {INSTALL_HINT}")
    if not stat.S_ISREG(status.st_mode) or not status.st_mode & 0o111:
        raise ValueError(f"Not an executable file: {path}")
    return resolved


def apfs_tool(name):
    for candidate in APFS_TOOLS[name]:
        if os.path.lexists(candidate):
            return trusted(candidate)
    raise ValueError(f"{name} is not installed at {' or '.join(APFS_TOOLS[name])}")


DIRECTORY_FLAGS = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
# Mount points live where only root can rename or replace an entry, so the path handed to
# mount(8) and apfs-fuse cannot be re-pointed between preparation and the mount syscall.
MOUNT_PARENT = "/run/darling-launcher"


def root_owned(status):
    return status.st_uid == 0 and not status.st_mode & 0o022


def open_child(parent_fd, name, mode=None, group=0):
    """Open a root-owned directory entry without following symlinks; with mode, create it and
    set root:group ownership and exactly that mode on the descriptor."""
    if mode is not None:
        try:
            os.mkdir(name, 0o700, dir_fd=parent_fd)
        except FileExistsError:
            pass
    try:
        descriptor = os.open(name, DIRECTORY_FLAGS, dir_fd=parent_fd)
    except OSError as error:
        raise ValueError(f"{name} is missing or is not a plain directory") from error
    try:
        if not root_owned(os.fstat(descriptor)):
            raise ValueError(f"{name} must be root-owned and not writable by others")
        if mode is not None:
            os.fchown(descriptor, 0, group)
            os.fchmod(descriptor, mode)
    except BaseException:
        os.close(descriptor)
        raise
    return descriptor


def caller_only_acl(uid):
    """system.posix_acl_access value: root rwx, the caller r-x, nobody else (Linux xattr ACL v2 layout)."""
    undefined = 0xFFFFFFFF
    entries = ((0x01, 7, undefined), (0x02, 5, uid), (0x04, 0, undefined), (0x10, 5, undefined), (0x20, 0, undefined))
    return struct.pack("<I", 2) + b"".join(struct.pack("<HHI", *entry) for entry in entries)


def restrict_to_caller(descriptor, uid, gid):
    """Let only the caller into parent/<uid>; without ACL support fall back to the primary group."""
    try:
        os.setxattr(descriptor, "system.posix_acl_access", caller_only_acl(uid))
        return True
    except OSError as error:
        if error.errno not in (errno.EOPNOTSUPP, errno.ENOTSUP):
            raise
    os.fchown(descriptor, 0, gid)
    return False


def open_mount_parent(uid, gid, parent=MOUNT_PARENT):
    """Open parent/<uid>, creating the last two levels; every component must be root-owned.
    parent is root:root 0755; parent/<uid> is root 0750 with an ACL for the caller only.
    Returns the descriptor and whether the ACL could be set."""
    parts = Path(parent).parts
    if not Path(parent).is_absolute() or len(parts) < 2 or ".." in parts:
        raise ValueError(f"Invalid mount parent: {parent}")
    descriptor = os.open("/", DIRECTORY_FLAGS)
    try:
        if not root_owned(os.fstat(descriptor)):
            raise ValueError("/ must be root-owned")
        names = list(parts[1:]) + [str(uid)]
        managed = {len(names) - 2: (0o755, 0), len(names) - 1: (0o750, 0)}
        for index, name in enumerate(names):
            child = open_child(descriptor, name, *managed.get(index, (None, 0)))
            os.close(descriptor)
            descriptor = child
        private = restrict_to_caller(descriptor, uid, gid)
    except BaseException:
        os.close(descriptor)
        raise
    return descriptor, private


def mount_directory(parent_fd, name):
    """Return a descriptor for an empty root-owned per-volume directory under parent_fd."""
    descriptor = open_child(parent_fd, name, 0o700)
    try:
        if os.listdir(descriptor):
            raise ValueError("Mount directory is mounted or not empty")
    except BaseException:
        os.close(descriptor)
        raise
    return descriptor


def same_directory(parent_fd, name, descriptor):
    current, prepared = os.stat(name, dir_fd=parent_fd, follow_symlinks=False), os.fstat(descriptor)
    return stat.S_ISDIR(current.st_mode) and (current.st_dev, current.st_ino) == (prepared.st_dev, prepared.st_ino)


def mounted_records(run_command):
    document = json.loads(run_command(["/usr/bin/findmnt", "--json", "--list", "--output", "SOURCE,TARGET,OPTIONS"]))
    return document.get("filesystems", [])


def release(target, run_command):
    """Unmount root's own target after a failed attempt, lazily if it is busy; returns the outcome."""
    for arguments in (["/usr/bin/umount", "--", target], ["/usr/bin/umount", "-l", "--", target]):
        try:
            run_command(arguments)
            return "unmounted"
        except RuntimeError:
            pass
    return "the mount point could not be unmounted"


def mount_batch(options, uid, gid, run_command=run, emit_event=emit, check_device=None, parent=MOUNT_PARENT):
    parent_fd, private = open_mount_parent(uid, gid, parent)
    try:
        if not private:
            emit_event("warning", message="POSIX ACLs are unavailable under the mount parent, so members of your primary group can reach these mounts")
        return mount_volumes(Path(parent) / str(uid), parent_fd, options, uid, gid, run_command, emit_event, check_device)
    finally:
        os.close(parent_fd)


def mount_volumes(root, root_fd, options, uid, gid, run_command, emit_event, check_device):
    inventory = partitions(json.loads(run_command(["/usr/bin/lsblk", "--json", "--output", "PATH,FSTYPE,LABEL,MOUNTPOINTS"])))
    selected, index, kernel = getattr(options, "device", None), getattr(options, "volume", None), getattr(options, "kernel", False)
    if selected is not None:
        inventory = [p for p in inventory if p.get("path") == selected]
        if not inventory:
            raise ValueError(f"{selected} is not a detected APFS/HFS partition")
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
            if partition["fstype"] == "apfs" and kernel:
                if index is None:
                    raise ValueError("Select an APFS container volume index explicitly for the kernel driver")
                driver = "/usr/bin/mount"
                entries = [{"index": index, "name": partition.get("label") or device, "encrypted": False}]
            elif partition["fstype"] == "apfs":
                utility, driver = apfs_tool("apfsutil"), apfs_tool("apfs-fuse")
                emit_event("progress", message=f"Enumerating {device}")
                entries = volumes(run_command([utility, device]))
                if index is not None:
                    entries = [v for v in entries if v["index"] == index]
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
                    if any(r.get("target") == str(destination) for r in existing):
                        raise ValueError("Mount directory is mounted or not empty")
                    prepared = mount_directory(root_fd, destination.name)
                    try:
                        if not same_directory(root_fd, destination.name, prepared):
                            raise ValueError("Mount directory changed before mounting")
                        flags = f"ro,nodev,nosuid,noexec,uid={uid},gid={gid}"
                        emit_event("progress", message=f"Mounting {device} volume {volume['index']} read-only")
                        if driver != "/usr/bin/mount":
                            # Root runs the FUSE daemon: allow_other lets the caller in, default_permissions
                            # makes the kernel enforce the uid=/gid= ownership for everyone else, and
                            # fsname makes the mount record name the device so it can be verified.
                            arguments = [driver, "-v", str(volume["index"]), "-o", flags + f",allow_other,default_permissions,fsname={device}", "--", device, str(destination)]
                        else:
                            options_text = flags + (f",vol={volume['index']}" if partition["fstype"] == "apfs" else "")
                            arguments = [driver, "-i", "-t", partition["fstype"], "-o", options_text, "--", device, str(destination)]
                        before = {(r.get("source"), r.get("target")) for r in existing}
                        try:
                            run_command(arguments)
                            failure = None
                        except RuntimeError as error:
                            failure = error
                    finally:
                        os.close(prepared)
                    # Checked even after a failed tool run, which may still have left a mount behind.
                    # Only root's own target is ever unmounted: other records naming the device can be
                    # created by anyone and point wherever their creator chose.
                    attempted = None
                    try:
                        records = mounted_records(run_command)
                        attempted = [r for r in records if r.get("target") == str(destination)
                                     and (r.get("source"), r.get("target")) not in before]
                        verified = next((r for r in attempted if r.get("source") == device), None)
                        if failure:
                            raise failure
                        if not verified:
                            raise RuntimeError("Mount did not verify as the requested device and target")
                        if not {"ro", "nodev", "nosuid", "noexec"}.issubset(set(verified.get("options", "").split(","))):
                            raise RuntimeError("Mount did not verify as read-only with requested restrictions")
                    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as error:
                        # Unless the table showed nothing at the target, take root's own mount off again.
                        outcome = "" if attempted == [] else "; " + release(str(destination), run_command)
                        raise RuntimeError(describe(error) + outcome) from None
                    existing = records
                    owned.append(str(destination))
                    emit_event("mounted", path=str(destination), device=device)
                except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as error:
                    emit_event("error", device=device, volume=volume["index"], message=describe(error))
        except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as error:
            emit_event("error", device=device, message=describe(error))
    # Only this run's mounts are inspected; root probing other mounts would reveal what the caller cannot read.
    sources = [path for path in owned if (Path(path) / "System/Library").is_dir() and ((Path(path) / "Applications").is_dir() or (Path(path) / "System/Applications").is_dir())]
    emit_event("complete", mounts=owned, sources=sources)
    return owned


def volume_index(text):
    value = int(text)
    if not 0 <= value < 100:
        raise ValueError(text)
    return value


def authorization_command(options):
    # Root runs only fixed, root-owned programs; nothing comes from PATH or a user-writable file.
    selection = ([f"--device={options.device}"] if options.device is not None else []) + ([f"--volume={options.volume}"] if options.volume is not None else []) + (["--kernel"] if options.kernel else [])
    return [trusted(PKEXEC), trusted(PYTHON), "-I", trusted(__file__), "--privileged", *selection]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", help="mount only this detected partition")
    parser.add_argument("--volume", type=volume_index, help="APFS container volume index (required with --kernel)")
    parser.add_argument("--kernel", action="store_true", help="use the kernel APFS driver instead of apfs-fuse")
    parser.add_argument("--privileged", action="store_true", help=argparse.SUPPRESS)
    options = parser.parse_args(argv)
    if options.device is not None and not options.device:
        parser.error("--device must not be empty")
    if options.device is None and (options.volume is not None or options.kernel):
        parser.error("--volume and --kernel apply only together with --device")
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
        os.umask(0o077)
        try:
            gid = pwd.getpwuid(uid).pw_gid
        except KeyError:
            raise ValueError("The calling user has no passwd entry") from None
        mount_batch(options, uid, gid)
        return 0
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as error:
        emit("error", message=describe(error))
        return 1


if __name__ == "__main__":
    sys.exit(main())
