#!/usr/bin/env python3
"""Check a real-day fidelity run against the committed numbers.

    python3 scripts/verify_real.py data/01302020.NASDAQ_ITCH50 [--build build] [--expected results/expected/01302020.json]

Runs nfe_fidelity (entry-order priority, the default) on the public Nasdaq file
and compares message count, event count, every class count, late inserts and
the audit totals with the expected file. Exits non-zero on any difference.
About 4 minutes and 2.5 GB of memory for a full day.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

ap = argparse.ArgumentParser()
ap.add_argument("itch")
ap.add_argument("--build", default="build")
ap.add_argument("--expected", default="results/expected/01302020.json")
a = ap.parse_args()

exp = json.load(open(a.expected))
with tempfile.TemporaryDirectory() as out:
    subprocess.run([os.path.join(a.build, "nfe_fidelity"), a.itch, "--out", out, "--priority", exp["priority"]], check=True)
    got = json.load(open(os.path.join(out, "fidelity.json")))

bad = 0
for key in ["messages", "events", "unknown_ref", "by_class", "late_inserts", "late_events", "audit", "audit_reconstruction"]:
    if got[key] != exp[key]:
        bad += 1
        print(f"DIFF {key}: expected {exp[key]}, got {got[key]}")
print("verify-real:", "FAILED" if bad else f"ok ({exp['events']:,} events, {exp['by_class']['unexplained']} unexplained)")
sys.exit(1 if bad else 0)
