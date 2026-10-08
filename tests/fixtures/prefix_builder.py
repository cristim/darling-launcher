# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic implementation of the external builder CLI for launcher tests."""
import argparse
import json
import os
from pathlib import Path
import shutil
import hashlib
import fcntl

parser = argparse.ArgumentParser()
parser.add_argument("command", choices=("resolve", "checkout", "resolve-nested", "checkout-nested", "build"))
parser.add_argument("--source")
parser.add_argument("--output")
parser.add_argument("--lock")
parser.add_argument("--workspace")
parser.add_argument("--jobs", type=int, default=1)
parser.add_argument("--cmake-arg", action="append", default=[])
args = parser.parse_args()
if args.command == "resolve":
    lock = {
        "schema": 1, "owner": "VibeDarling", "complete": True,
        "repos": [{"repo": "darling", "branch": "master", "base": "a" * 40,
                   "prs": [{"number": 7, "head": "b" * 40, "ref": "refs/pull/7/head"}]}],
        "fail_checkout": (Path(args.source) / "fail-checkout").exists(),
    }
    Path(args.output).write_text(json.dumps(lock))
elif args.command == "checkout":
    lock = json.loads(Path(args.lock).read_text())
    if lock.get("fail_checkout"):
        raise SystemExit(3)
    workspace = Path(args.workspace)
    workspace.mkdir(exist_ok=False)
    shutil.copyfile(args.lock, workspace / "refs.lock.json")
    (workspace / "integrated.json").write_text(json.dumps({"repositories": {"darling": "a" * 40}}))
elif args.command == "resolve-nested":
    workspace = Path(args.workspace)
    lock = {"schema": 1, "owner": "VibeDarling",
            "top_lock_sha256": hashlib.sha256((workspace / "refs.lock.json").read_bytes()).hexdigest(),
            "vibedarling": [{"repo": "nested", "branch": "main", "base": "c" * 40,
                            "prs": [{"number": 2, "head": "d" * 40}]}], "external_pinned": []}
    Path(args.output).write_text(json.dumps(lock))
elif args.command == "checkout-nested":
    workspace = Path(args.workspace)
    shutil.copyfile(args.lock, workspace / "nested.refs.lock.json")
    (workspace / "nested.integrated.json").write_text("[]")
elif args.command == "build":
    with open(os.environ.get("DARLING_LAUNCHER_LOCK_DIR", "/tmp/agent-locks") + "/darling-heavy-build.lock", "a") as shared_lock:
        try:
            fcntl.flock(shared_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            pass
        else:
            raise RuntimeError("Host adapter did not hold the shared heavy-build lock")
    workspace = Path(args.workspace)
    (workspace / "prefix/private/etc").mkdir(parents=True)
    (workspace / "prefix/private/etc/passwd").write_text("synthetic fixture\n")
    (workspace / "image/usr/local").mkdir(parents=True)
    launcher = workspace / "build/src/startup/darling"
    launcher.parent.mkdir(parents=True)
    launcher.write_text('#!/bin/sh\nprintf "%s\\n" "$DARLING_INSTALL_PREFIX" > "$DPREFIX/runtime-used.txt"\nexit 0\n')
    launcher.chmod(0o700)
    (workspace / "build-call.json").write_text(json.dumps({"jobs": args.jobs, "cmake_args": args.cmake_arg}))
print("fixture completed:", args.command, flush=True)
