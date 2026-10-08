#!/usr/bin/env python3
"""Lock and integrate VibeDarling default branches plus their open PR heads.

The lock is a point-in-time input list. Checkout creates an independent clone;
it never updates an existing checkout, build directory, or prefix.
"""

import argparse
import concurrent.futures
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request


OWNER = "VibeDarling"
ROOT = "https://github.com/VibeDarling/darling.git"
SHA = re.compile(r"^[0-9a-f]{40}$")


def run(*args, cwd=None, env=None):
    p = subprocess.run(args, cwd=cwd, env=env, text=True, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    if p.returncode:
        raise RuntimeError(f"{' '.join(map(str, args))}: {p.stderr.strip()}")
    return p.stdout.strip()


def api(path):
    headers = {"Accept": "application/vnd.github+json", "User-Agent": "darling-pr-prefix"}
    if os.environ.get("GITHUB_TOKEN"):
        headers["Authorization"] = "Bearer " + os.environ["GITHUB_TOKEN"]
    try:
        with urllib.request.urlopen(urllib.request.Request(
                "https://api.github.com" + path, headers=headers), timeout=30) as response:
            return json.load(response)
    except urllib.error.HTTPError as error:
        raise RuntimeError(f"GitHub API {path}: HTTP {error.code}: {error.read().decode()}") from error


def modules(root, allow_external=False, required_prefix="src/external/"):
    text = run("git", "show", "HEAD:.gitmodules", cwd=root)
    data = []
    section = None
    for line in text.splitlines():
        match = re.match(r'^\[submodule "([^"]+)"\]$', line)
        if match:
            if section:
                data.append(section)
            section = {"name": match.group(1)}
        elif section and "=" in line:
            key, value = line.strip().split("=", 1)
            section[key.strip()] = value.strip()
    if section:
        data.append(section)
    paths = set()
    result = []
    by_repo = {}
    for item in data:
        path, url = item.get("path"), item.get("url")
        if not path or not url or path in paths or path.startswith("/") or ".." in Path(path).parts or (required_prefix and not path.startswith(required_prefix)):
            raise RuntimeError(f"invalid .gitmodules entry: {item}")
        paths.add(path)
        if re.fullmatch(r"\.\./[A-Za-z0-9_.-]+(?:\.git)?", url):
            repo = url[3:].removesuffix(".git")
            clone_url = f"https://github.com/{OWNER}/{repo}.git"
            kind = "VibeDarling"
        elif allow_external and re.fullmatch(
                r"https://github\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+(?:\.git)?", url):
            repo = url.removeprefix("https://github.com/").removesuffix(".git")
            clone_url = url
            kind = "external"
            if repo.lower().startswith(OWNER.lower() + "/"):
                raise RuntimeError(f"unexpected absolute VibeDarling URL: {url}")
        else:
            raise RuntimeError(f"unsupported submodule URL (scope must be audited): {url}")
        requested_branch = item.get("branch")
        key = (repo.lower(), requested_branch)
        if key in by_repo:
            by_repo[key]["paths"].append(path)
        else:
            entry = {"repo": repo, "paths": [path], "url": clone_url, "kind": kind}
            if requested_branch:
                entry["requested_branch"] = requested_branch
            by_repo[key] = entry
            result.append(entry)
    return result


def open_prs():
    found = []
    page = 1
    while True:
        query = urllib.parse.urlencode({"q": f"org:{OWNER} is:pr is:open",
                                        "per_page": 100, "page": page})
        response = api("/search/issues?" + query)
        if response.get("incomplete_results"):
            raise RuntimeError("GitHub search was incomplete")
        found += response["items"]
        if len(found) >= response["total_count"]:
            break
        if page == 10:
            raise RuntimeError("GitHub search exceeded its 1000-result limit")
        page += 1
    if len({(p["repository_url"], p["number"]) for p in found}) != len(found):
        raise RuntimeError("duplicate PR in GitHub search")
    return found


def remote_refs(item):
    patterns = ["HEAD", "refs/heads/main", "refs/heads/master"]
    requested = item.get("requested_branch")
    if requested:
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_./-]*", requested) or ".." in requested:
            raise RuntimeError(f"invalid declared branch for {item['repo']}: {requested}")
        patterns.append(f"refs/heads/{requested}")
    patterns += [f"refs/pull/{p['number']}/head" for p in item["prs"]]
    output = run("git", "ls-remote", "--symref", item["url"], *patterns)
    refs = {}
    default = None
    for line in output.splitlines():
        if line.startswith("ref: "):
            target, name = line[5:].split("\t", 1)
            if name == "HEAD":
                default = target
        else:
            commit, name = line.split("\t", 1)
            if not SHA.fullmatch(commit):
                raise RuntimeError(f"invalid ref SHA in {item['repo']}: {line}")
            refs[name] = commit
    if not default or not default.startswith("refs/heads/"):
        raise RuntimeError(f"{item['repo']}: cannot resolve default branch: {default}")
    if default not in refs and default not in ("refs/heads/main", "refs/heads/master"):
        refs[default] = refs.get("HEAD")
    if not refs.get(default) or refs.get(default) != refs.get("HEAD"):
        raise RuntimeError(f"{item['repo']}: missing or inconsistent default HEAD")
    selected = f"refs/heads/{requested}" if requested else default
    if selected not in refs:
        raise RuntimeError(f"{item['repo']}: declared branch {requested} is missing")
    item["branch"] = selected.removeprefix("refs/heads/")
    item["base"] = refs[selected]
    if item["branch"] not in ("main", "master"):
        item["branch_exception"] = ("declared version branch" if requested else
                                    "default branch has no main/master ref")
    for pr in item["prs"]:
        ref = f"refs/pull/{pr['number']}/head"
        if ref not in refs:
            raise RuntimeError(f"{item['repo']} PR #{pr['number']}: missing {ref}")
        pr["head"] = refs[ref]
        pr["ref"] = ref
    return item


def resolve(args):
    source = Path(args.source).resolve()
    if run("git", "remote", "get-url", "origin", cwd=source) != ROOT:
        raise RuntimeError("source origin must be the VibeDarling superproject")
    if run("git", "status", "--porcelain", cwd=source):
        raise RuntimeError("source must be clean")
    items = [{"repo": "darling", "paths": ["."], "url": ROOT}] + modules(source)
    by_repo = {}
    for item in items:
        by_repo.setdefault(item["repo"].lower(), []).append(item)
    excluded = []
    for pr in open_prs():
        repo = pr["repository_url"].rsplit("/", 1)[-1]
        key = repo.lower()
        record = {"number": pr["number"], "url": pr["html_url"],
                  "title": pr["title"]}
        if key in by_repo:
            choices = by_repo[key]
            if len(choices) > 1:
                detail = api(f"/repos/{OWNER}/{repo}/pulls/{pr['number']}")
                target = detail["base"]["ref"]
                choices = [item for item in choices if item.get("requested_branch") == target]
                if not choices:
                    raise RuntimeError(f"{repo} PR #{pr['number']} targets untracked branch {target}")
            choices[0].setdefault("prs", []).append(record)
        else:
            excluded.append({"repo": repo, **record, "reason": "not a superproject or submodule"})
    for item in items:
        item.setdefault("prs", [])
        item["prs"].sort(key=lambda p: p["number"])
    if args.repo:
        chosen = {r.lower() for r in args.repo}
        if chosen - by_repo.keys():
            raise RuntimeError(f"unknown repository: {sorted(chosen - by_repo.keys())}")
        items = [item for item in items if item["repo"].lower() in chosen]
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        items = list(pool.map(remote_refs, items))
    if not args.repo and items[0]["base"] != run("git", "rev-parse", "HEAD", cwd=source):
        raise RuntimeError("source HEAD is stale; fetch and check out current VibeDarling master")
    lock = {"schema": 1, "owner": OWNER,
            "resolved_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "complete": not bool(args.repo), "repos": items,
            "excluded_open_prs": excluded}
    out = Path(args.output).resolve()
    if out.exists():
        raise RuntimeError(f"refusing to overwrite lock: {out}")
    out.write_text(json.dumps(lock, indent=2) + "\n")
    print(f"locked {len(items)} checkout entries, "
          f"{len({i['repo'].lower() for i in items})} repositories, "
          f"and {sum(len(i['prs']) for i in items)} PRs: {out}")
    print(f"excluded {len(excluded)} open PRs outside the dependency tree")


def object_at(repo, sha, fallback):
    try:
        run("git", "cat-file", "-e", sha + "^{commit}", cwd=repo)
        return
    except RuntimeError:
        pass
    try:
        run("git", "fetch", "origin", sha, cwd=repo)
    except RuntimeError:
        run("git", "fetch", "origin", fallback, cwd=repo)
    run("git", "cat-file", "-e", sha + "^{commit}", cwd=repo)


def integrate(repo, item):
    object_at(repo, item["base"], f"refs/heads/{item['branch']}")
    run("git", "checkout", "--detach", item["base"], cwd=repo)
    for pr in item["prs"]:
        object_at(repo, pr["head"], pr["ref"])
        env = dict(os.environ, GIT_AUTHOR_NAME="Darling PR integration",
                   GIT_AUTHOR_EMAIL="integration@localhost",
                   GIT_COMMITTER_NAME="Darling PR integration",
                   GIT_COMMITTER_EMAIL="integration@localhost")
        run("git", "merge", "--no-ff", "--no-edit", "-m",
            f"integration: merge {item['repo']} PR #{pr['number']} ({pr['head']})",
            pr["head"], cwd=repo, env=env)
    return run("git", "rev-parse", "HEAD", cwd=repo)


def clone_and_integrate(source, item, seed_root):
    commits = []
    for path in item["paths"]:
        target = source / path
        target.parent.mkdir(parents=True, exist_ok=True)
        seed = seed_root / path if seed_root else None
        if seed and (seed / ".git").exists():
            run("git", "clone", "--no-local", "--no-checkout", str(seed), str(target))
            run("git", "remote", "set-url", "origin", item["url"], cwd=target)
        else:
            run("git", "clone", "--depth=1", "--no-checkout", item["url"], str(target))
        commits.append(integrate(target, item))
    if len(set(commits)) != 1:
        raise RuntimeError(f"repeated checkout paths diverged for {item['repo']}")
    return item, commits[0]


def checkout(args):
    lock = json.loads(Path(args.lock).read_text())
    if lock.get("schema") != 1 or lock.get("owner") != OWNER or not lock.get("complete"):
        raise RuntimeError("checkout requires a complete VibeDarling schema-1 lock")
    items = lock["repos"]
    if not items or items[0]["repo"] != "darling":
        raise RuntimeError("lock has no superproject")
    workspace = Path(args.workspace).resolve()
    if workspace.exists():
        raise RuntimeError(f"refusing existing workspace: {workspace}")
    workspace.mkdir(parents=True)
    shutil.copy2(args.lock, workspace / "refs.lock.json")
    source = workspace / "source"
    if args.seed_superproject:
        seed = Path(args.seed_superproject).resolve()
        if run("git", "remote", "get-url", "origin", cwd=seed) != ROOT:
            raise RuntimeError("seed clone must have VibeDarling origin")
        run("git", "clone", "--no-local", "--no-checkout", str(seed), str(source))
        run("git", "remote", "set-url", "origin", ROOT, cwd=source)
    else:
        run("git", "clone", "--depth=1", "--no-checkout", ROOT, str(source))
    integrated = {".": integrate(source, items[0])}
    expected = {(x["repo"].lower(), p, x["url"]) for x in items[1:] for p in x["paths"]}
    merged_modules = modules(source, allow_external=True)
    actual = {(x["repo"].lower(), p, x["url"]) for x in merged_modules for p in x["paths"]}
    if not expected.issubset(actual):
        raise RuntimeError("superproject PRs removed or changed locked .gitmodules entries")
    additions = [x for x in merged_modules if x["kind"] == "external"]
    if actual - expected != {(x["repo"].lower(), p, x["url"])
                            for x in additions for p in x["paths"]}:
        raise RuntimeError("superproject PRs added a VibeDarling submodule absent from the lock")
    seed_root = Path(args.seed_submodules_root).resolve() if args.seed_submodules_root else None
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for start in range(1, len(items), args.jobs):
            batch = items[start:start + args.jobs]
            futures = {pool.submit(clone_and_integrate, source, item, seed_root): item
                       for item in batch}
            for future in concurrent.futures.as_completed(futures):
                try:
                    item, sha = future.result()
                except RuntimeError as error:
                    raise RuntimeError(f"{futures[future]['repo']}: {error}") from error
                for path in item["paths"]:
                    integrated[path] = sha
                for path in item["paths"]:
                    run("git", "add", "--", path, cwd=source)
                print(f"integrated {item['repo']}: {sha}", flush=True)
    external = []
    for item in additions:
        for path in item["paths"]:
            line = run("git", "ls-tree", "HEAD", "--", path, cwd=source)
            match = re.fullmatch(r"160000 commit ([0-9a-f]{40})\t(.+)", line)
            if not match or match.group(2) != path:
                raise RuntimeError(f"PR-added submodule has no gitlink commit: {path}")
            sha = match.group(1)
            target = source / path
            target.parent.mkdir(parents=True, exist_ok=True)
            run("git", "clone", "--depth=1", "--no-checkout",
                item["url"], str(target))
            object_at(target, sha, "HEAD")
            run("git", "checkout", "--detach", sha, cwd=target)
            external.append({"path": path, "url": item["url"], "gitlink": sha})
    env = dict(os.environ, GIT_AUTHOR_NAME="Darling PR integration",
               GIT_AUTHOR_EMAIL="integration@localhost",
               GIT_COMMITTER_NAME="Darling PR integration",
               GIT_COMMITTER_EMAIL="integration@localhost")
    if run("git", "diff", "--cached", "--name-only", cwd=source):
        run("git", "commit", "-m", "integration: pin VibeDarling default branches and open PRs",
            cwd=source, env=env)
    (workspace / "integrated.json").write_text(json.dumps(
        {"repositories": integrated, "pr_added_external_submodules": external}, indent=2) + "\n")
    print(f"integrated source: {source}")
    print(f"superproject commit: {run('git', 'rev-parse', 'HEAD', cwd=source)}")


def nested_modules(workspace):
    source = workspace / "source"
    top = json.loads((workspace / "refs.lock.json").read_text())
    found = []
    for item in top["repos"][1:]:
        for parent_path in item["paths"]:
            parent = source / parent_path
            if (parent / ".gitmodules").is_file():
                for module in modules(parent, allow_external=True, required_prefix=None):
                    for subpath in module["paths"]:
                        line = run("git", "ls-tree", "HEAD", "--", subpath, cwd=parent)
                        match = re.fullmatch(r"160000 commit ([0-9a-f]{40})\t(.+)", line)
                        if not match or match.group(2) != subpath:
                            raise RuntimeError(f"nested module has no gitlink: {parent_path}/{subpath}")
                        found.append({"parent": parent_path, "path": subpath,
                                      "repo": module["repo"], "url": module["url"],
                                      "kind": module["kind"], "pin": match.group(1)})
    return found


def resolve_nested(args):
    workspace = Path(args.workspace).resolve()
    if not (workspace / "integrated.json").is_file():
        raise RuntimeError("complete top-level checkout first")
    occurrences = nested_modules(workspace)
    by_repo = {}
    external = []
    for occurrence in occurrences:
        if occurrence["kind"] == "external":
            external.append(occurrence)
        else:
            key = occurrence["repo"].lower()
            if key not in by_repo:
                by_repo[key] = {"repo": occurrence["repo"], "url": occurrence["url"],
                                "occurrences": [], "prs": []}
            by_repo[key]["occurrences"].append(occurrence)
    top = json.loads((workspace / "refs.lock.json").read_text())
    top_repos = {item["repo"].lower() for item in top["repos"]}
    prs = open_prs()
    for pr in prs:
        repo = pr["repository_url"].rsplit("/", 1)[-1].lower()
        if repo in by_repo:
            by_repo[repo]["prs"].append({"number": pr["number"],
                                         "url": pr["html_url"], "title": pr["title"]})
        elif repo not in top_repos and not any(
                p["repo"].lower() == repo for p in top["excluded_open_prs"]):
            raise RuntimeError(f"PR set changed since top-level lock: {pr['html_url']}")
    items = list(by_repo.values())
    for item in items:
        item["prs"].sort(key=lambda p: p["number"])
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        items = list(pool.map(remote_refs, items))
    lock = {"schema": 1, "owner": OWNER,
            "resolved_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "top_lock_sha256": hashlib.sha256((workspace / "refs.lock.json").read_bytes()).hexdigest(),
            "vibedarling": items, "external_pinned": external}
    output = Path(args.output).resolve()
    if output.exists():
        raise RuntimeError(f"refusing to overwrite nested lock: {output}")
    output.write_text(json.dumps(lock, indent=2) + "\n")
    print(f"locked {len(items)} nested VibeDarling repos and {len(external)} external pins: {output}")


def checkout_nested(args):
    workspace = Path(args.workspace).resolve()
    source = workspace / "source"
    lock = json.loads(Path(args.lock).read_text())
    if lock.get("schema") != 1 or lock.get("owner") != OWNER:
        raise RuntimeError("invalid nested lock")
    digest = hashlib.sha256((workspace / "refs.lock.json").read_bytes()).hexdigest()
    if digest != lock.get("top_lock_sha256"):
        raise RuntimeError("nested lock belongs to a different top-level lock")
    if (workspace / "nested.integrated.json").exists():
        raise RuntimeError("nested integration already completed")
    current = nested_modules(workspace)
    expected = [o for item in lock["vibedarling"] for o in item["occurrences"]]
    expected += lock["external_pinned"]
    if sorted(current, key=lambda x: (x["parent"], x["path"])) != sorted(
            expected, key=lambda x: (x["parent"], x["path"])):
        raise RuntimeError("nested submodule map changed since nested lock")
    changed_parents = set()
    realized = []
    for item in lock["vibedarling"]:
        for o in item["occurrences"]:
            parent = source / o["parent"]
            target = parent / o["path"]
            target.parent.mkdir(parents=True, exist_ok=True)
            run("git", "clone", "--depth=1", "--no-checkout", item["url"], str(target))
            sha = integrate(target, item)
            if (target / ".gitmodules").is_file():
                raise RuntimeError(f"deeper nested modules need a new integration step: {target}")
            run("git", "add", "--", o["path"], cwd=parent)
            changed_parents.add(o["parent"])
            realized.append({"path": str(Path(o["parent"]) / o["path"]),
                             "repo": item["repo"], "commit": sha})
    for o in lock["external_pinned"]:
        parent = source / o["parent"]
        target = parent / o["path"]
        target.parent.mkdir(parents=True, exist_ok=True)
        run("git", "clone", "--depth=1", "--no-checkout", o["url"], str(target))
        object_at(target, o["pin"], "HEAD")
        run("git", "checkout", "--detach", o["pin"], cwd=target)
        if (target / ".gitmodules").is_file():
            raise RuntimeError(f"deeper nested modules need a new integration step: {target}")
        realized.append({"path": str(Path(o["parent"]) / o["path"]),
                         "repo": o["repo"], "commit": o["pin"]})
    env = dict(os.environ, GIT_AUTHOR_NAME="Darling PR integration",
               GIT_AUTHOR_EMAIL="integration@localhost",
               GIT_COMMITTER_NAME="Darling PR integration",
               GIT_COMMITTER_EMAIL="integration@localhost")
    for parent_path in changed_parents:
        parent = source / parent_path
        if run("git", "diff", "--cached", "--name-only", cwd=parent):
            run("git", "commit", "-m", "integration: pin nested VibeDarling repositories",
                cwd=parent, env=env)
            run("git", "add", "--", parent_path, cwd=source)
    if run("git", "diff", "--cached", "--name-only", cwd=source):
        run("git", "commit", "-m", "integration: pin nested VibeDarling repositories",
            cwd=source, env=env)
    shutil.copy2(args.lock, workspace / "nested.refs.lock.json")
    (workspace / "nested.integrated.json").write_text(json.dumps(realized, indent=2) + "\n")
    print(f"integrated {len(realized)} nested paths; superproject {run('git', 'rev-parse', 'HEAD', cwd=source)}")


def build(args):
    workspace = Path(args.workspace).resolve()
    source = workspace / "source"
    if not (workspace / "integrated.json").is_file() or not source.is_dir():
        raise RuntimeError("workspace has not completed checkout")
    if nested_modules(workspace) and not (workspace / "nested.integrated.json").is_file():
        raise RuntimeError("resolve-nested and checkout-nested before building")
    if run("git", "status", "--porcelain", cwd=source):
        raise RuntimeError("integrated source is dirty")
    builddir, image, prefix = (workspace / name for name in ("build", "image", "prefix"))
    if image.exists() or prefix.exists():
        raise RuntimeError("image and prefix must not exist yet")
    marker = builddir / ".darling-pr-prefix-source"
    source_sha = run("git", "rev-parse", "HEAD", cwd=source)
    if builddir.exists():
        if not marker.is_file() or marker.read_text().strip() != source_sha:
            raise RuntimeError("existing build directory does not match this integrated source")
        if args.cmake_arg:
            raise RuntimeError("CMake arguments can only be supplied on first configuration")
    else:
        run("cmake", "-S", str(source), "-B", str(builddir), "-G", "Ninja",
            "-DCMAKE_INSTALL_PREFIX=/usr/local", *args.cmake_arg)
        marker.write_text(source_sha + "\n")
    if args.configure_only:
        print(f"configured: {builddir} ({source_sha})")
        return
    run("cmake", "--build", str(builddir), "--parallel", str(args.jobs))
    pending = run("ninja", "-C", str(builddir), "-n")
    if "no work to do" not in pending.lower():
        run("cmake", "--build", str(builddir), "--parallel", str(args.jobs))
        if "no work to do" not in run("ninja", "-C", str(builddir), "-n").lower():
            raise RuntimeError("build graph still has pending work")
    run("cmake", "--install", str(builddir), env=dict(os.environ, DESTDIR=str(image)))
    launcher = builddir / "src/startup/darling"
    env = dict(os.environ, DPREFIX=str(prefix), DARLING_INSTALL_PREFIX=str(image / "usr/local"))
    run(str(launcher), "shell", "true", env=env)
    run(str(launcher), "shutdown", env=env)
    if not (prefix / "private/etc/passwd").is_file():
        raise RuntimeError("launcher did not initialize a usable prefix")
    print(f"prefix: {prefix}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    discover = commands.add_parser("resolve", help="lock live VibeDarling refs")
    discover.add_argument("--source", default=".")
    discover.add_argument("--output", required=True)
    discover.add_argument("--repo", action="append", help="limited diagnostic resolution only")
    discover.add_argument("--jobs", type=int, default=8)
    materialize = commands.add_parser("checkout", help="clone and integrate locked refs")
    materialize.add_argument("--lock", required=True)
    materialize.add_argument("--workspace", required=True)
    materialize.add_argument("--jobs", type=int, default=6)
    materialize.add_argument("--seed-superproject", help="existing independent clone to copy locally")
    materialize.add_argument("--seed-submodules-root", help="read-only populated tree to copy with --no-local")
    nested_discover = commands.add_parser("resolve-nested", help="lock nested submodule refs")
    nested_discover.add_argument("--workspace", required=True)
    nested_discover.add_argument("--output", required=True)
    nested_discover.add_argument("--jobs", type=int, default=8)
    nested_materialize = commands.add_parser("checkout-nested", help="integrate locked nested refs")
    nested_materialize.add_argument("--workspace", required=True)
    nested_materialize.add_argument("--lock", required=True)
    compile_command = commands.add_parser("build", help="build and initialize an isolated prefix")
    compile_command.add_argument("--workspace", required=True)
    compile_command.add_argument("--jobs", type=int, default=4)
    compile_command.add_argument("--cmake-arg", action="append", default=[])
    compile_command.add_argument("--configure-only", action="store_true")
    args = parser.parse_args()
    if getattr(args, "jobs", 1) < 1:
        parser.error("--jobs must be positive")
    try:
        {"resolve": resolve, "checkout": checkout,
         "resolve-nested": resolve_nested, "checkout-nested": checkout_nested,
         "build": build}[args.command](args)
    except (RuntimeError, OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
