#!/usr/bin/env python3
"""Test helper: print the reference oracle's check_text() hits (plus recon_format() for recon-* files) for every *.txt file in a directory as
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
            text = f.read()
        hits = check_handoff.check_text(text, "research", "spec")
        if "recon-" in name:
            hits = sorted(hits + check_handoff.recon_format(text))
        out[name] = [list(h) for h in hits]
json.dump(out, sys.stdout)
