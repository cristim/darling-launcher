#!/usr/bin/env python3
"""Test helper: print the reference oracle's check_text() hits for every *.txt file in a directory as
JSON {file: [[line, rule, description], ...]}. Used only by launcher-handoff-tests when python3 exists."""
import json
import os
import sys

oracle_dir, corpus = sys.argv[1], sys.argv[2]
sys.path.insert(0, oracle_dir)
import check_handoff  # noqa: E402

out = {}
for name in sorted(os.listdir(corpus)):
    if name.endswith(".txt"):
        with open(os.path.join(corpus, name), encoding="utf-8", newline="") as f:
            out[name] = [list(h) for h in check_handoff.check_text(f.read(), "research", "spec")]
json.dump(out, sys.stdout)
