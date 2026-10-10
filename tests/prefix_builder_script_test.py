#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("prefix_builder", Path(__file__).resolve().parents[1] / "tools/all-vibedarling-pr-prefix.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)

SHA = "a" * 40
PR = {"repository_url": "https://api.github.com/repos/VibeDarling/darling", "number": 7,
      "html_url": "https://github.com/VibeDarling/darling/pull/7", "title": "outside contribution"}


def fake_git(*args, cwd=None, env=None):
    if args[:3] == ("git", "remote", "get-url"):
        return builder.ROOT
    if args[:2] == ("git", "rev-parse"):
        return SHA
    return ""


def main(*arguments):
    with patch.object(sys, "argv", ["all-vibedarling-pr-prefix.py", *arguments]):
        return builder.main()


class PrefixBuilderScriptTest(unittest.TestCase):
    def resolve(self, *extra):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "lock.json"
            with patch.object(builder, "run", fake_git), patch.object(builder, "modules", return_value=[]), \
                 patch.object(builder, "remote_refs", side_effect=lambda item: {**item, "base": SHA, "branch": "master"}), \
                 patch.object(builder, "open_prs", return_value=[PR]) as inventory:
                self.assertEqual(main("resolve", "--source", directory, "--output", str(output), *extra), 0)
            return json.loads(output.read_text()), inventory

    def test_open_prs_are_not_locked_by_default(self):
        lock, inventory = self.resolve()
        inventory.assert_not_called()
        self.assertEqual(lock["repos"][0]["prs"], [])
        self.assertFalse(lock["include_prs"])

    def test_open_prs_are_locked_only_on_request(self):
        lock, inventory = self.resolve("--include-prs")
        inventory.assert_called_once()
        self.assertEqual([p["number"] for p in lock["repos"][0]["prs"]], [7])
        self.assertTrue(lock["include_prs"])

    def test_build_children_never_receive_the_github_token(self):
        with tempfile.TemporaryDirectory() as directory:
            workspace = Path(directory)
            (workspace / "source").mkdir()
            (workspace / "integrated.json").write_text("{}")
            (workspace / "refs.lock.json").write_text(json.dumps({"repos": [{"repo": "darling"}]}))
            environments = []

            def fake_run(arguments, cwd=None, env=None, **kwargs):
                environments.append((arguments[0], os.environ if env is None else env))
                output = ""
                if arguments[:2] == ("git", "rev-parse"):
                    output = SHA
                elif arguments[:2] == ("cmake", "-S"):
                    Path(arguments[arguments.index("-B") + 1]).mkdir()
                elif arguments[0] == "ninja":
                    output = "ninja: no work to do."
                elif arguments[0].endswith("src/startup/darling") and arguments[1] == "shell":
                    (Path(env["DPREFIX"]) / "private/etc").mkdir(parents=True)
                    (Path(env["DPREFIX"]) / "private/etc/passwd").write_text("")
                return subprocess.CompletedProcess(arguments, 0, output, "")

            secrets = {"GITHUB_TOKEN": "t", "GH_TOKEN": "t", "GH_ENTERPRISE_TOKEN": "t", "GITHUB_ENTERPRISE_TOKEN": "t",
                       "GITHUB_PAT": "t", "GIT_ASKPASS": "/x", "SSH_AUTH_SOCK": "/x", "AWS_SECRET_ACCESS_KEY": "t",
                       "GIT_CONFIG_COUNT": "1", "GIT_CONFIG_KEY_0": "credential.helper", "GIT_CONFIG_VALUE_0": "!evil"}
            with patch.dict(os.environ, {**secrets, "LC_ALL": "C"}), \
                 patch.object(builder.subprocess, "run", fake_run):
                self.assertEqual(main("build", "--workspace", directory, "--jobs", "1"), 0)
            programs = {program for program, _ in environments}
            self.assertTrue({"cmake", "ninja"} <= programs)
            self.assertTrue(any(p.endswith("src/startup/darling") for p in programs))
            for program, env in environments:
                self.assertFalse(secrets.keys() & env.keys(), program)
                self.assertEqual(env["LC_ALL"], "C")
                self.assertEqual(env["PATH"], os.environ["PATH"])


if __name__ == "__main__":
    unittest.main()
