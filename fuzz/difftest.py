"""Differential fuzzing: the C++ engine against the OCaml executable spec.

A structured generator writes command segments in the shared text protocol
(see src/tools/nfe_engine.cpp). Prices cluster on a few ticks so queues form
and orders cross often; cancels, deletes and replaces target live orders;
a few commands are invalid on purpose. Each segment ends with "#", which
dumps the book and starts a fresh one, so one process run covers many
independent segments.

Both engines must print identical fills, rejects and final books. On a
mismatch, the failing segment is shrunk by delta debugging and saved under
tests/regressions/ so it becomes a permanent regression test.

    python3 fuzz/difftest.py --ops 1000000 --seed 7
"""
import argparse
import json
import os
import random
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CPP = os.path.join(ROOT, "build", "nfe_engine")
SPEC = os.path.join(ROOT, "spec", "_build", "default", "bin", "spec_engine.exe")


def gen_segment(rng, n):
    lines, live, next_id = [], [], 1
    base = rng.randint(50, 5000)
    width = rng.choice((1, 2, 3, 5))
    for _ in range(n):
        k = rng.random()
        if k < 0.55 or not live:
            side = rng.choice("BS")
            px = base + rng.randint(-width, width)
            qty = rng.choice((1, 50, 100, 100, 100, 200, 300, 500, 1000))
            lines.append(f"A {next_id} {side} {px} {qty}")
            live.append(next_id)
            next_id += 1
        elif k < 0.70:
            lines.append(f"X {rng.choice(live)} {rng.choice((1, 50, 100, 250, 1000))}")
        elif k < 0.82:
            lines.append(f"D {rng.choice(live)}")
        elif k < 0.95:
            old = rng.choice(live)
            px = base + rng.randint(-width, width)
            lines.append(f"U {old} {next_id} {px} {rng.choice((50, 100, 200, 300))}")
            live.append(next_id)
            next_id += 1
        else:  # invalid on purpose: unknown id, zero size, duplicate id
            lines.append(rng.choice((f"D {next_id + 1000}", f"A {next_id} B {base} 0",
                                     f"A {live[0]} S {base} 100", f"X {next_id + 7} 10")))
        if len(live) > 64:
            live = live[-64:]
    return lines


def run(exe, text):
    p = subprocess.run([exe], input=text, capture_output=True, text=True, timeout=600)
    if p.returncode != 0:
        return None
    return p.stdout


def segments(out):
    return out.split("#\n")


def first_mismatch(cpp, spec, segs):
    text = "".join("\n".join(s) + "\n#\n" for s in segs)
    a, b = run(cpp, text), run(spec, text)
    if a is None:
        return 0, "cpp crashed"
    if b is None:
        return 0, "spec crashed"
    sa, sb = segments(a), segments(b)
    for i, (x, y) in enumerate(zip(sa, sb)):
        if x != y:
            return i, None
    if len(sa) != len(sb):
        return min(len(sa), len(sb)), None
    return -1, None


def differs(cpp, spec, seg):
    i, err = first_mismatch(cpp, spec, [seg])
    return i >= 0 or err is not None


def shrink(cpp, spec, seg):
    """Delta debugging: drop chunks while the mismatch survives."""
    n = 2
    while len(seg) >= 2:
        chunk = max(1, len(seg) // n)
        reduced = False
        for start in range(0, len(seg), chunk):
            cand = seg[:start] + seg[start + chunk:]
            if cand and differs(cpp, spec, cand):
                seg, n, reduced = cand, max(n - 1, 2), True
                break
        if not reduced:
            if chunk == 1:
                break
            n = min(len(seg), n * 2)
    return seg


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ops", type=int, default=100_000)
    ap.add_argument("--seg", type=int, default=200, help="operations per segment")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--cpp", default=CPP)
    ap.add_argument("--spec", default=SPEC)
    ap.add_argument("--no-shrink", action="store_true")
    ap.add_argument("--save", default=os.path.join(ROOT, "tests", "regressions"))
    ap.add_argument("--json", help="write the result here")
    ap.add_argument("--regressions", action="store_true", help="only replay saved failing cases")
    a = ap.parse_args()

    if a.regressions:
        import glob
        bad = [p for p in sorted(glob.glob(os.path.join(a.save, "*.txt")))
               if differs(a.cpp, a.spec, open(p).read().split("\n"))]
        print(f"{len(glob.glob(os.path.join(a.save, '*.txt')))} regression cases, {len(bad)} failing {bad}")
        sys.exit(1 if bad else 0)

    rng = random.Random(a.seed)
    segs = [gen_segment(rng, a.seg) for _ in range(max(1, a.ops // a.seg))]
    i, err = first_mismatch(a.cpp, a.spec, segs)
    result = {"ops": len(segs) * a.seg, "seed": a.seed, "caught": i >= 0 or err is not None,
              "ops_to_detection": None, "error": err}
    if result["caught"]:
        result["ops_to_detection"] = (max(i, 0) + 1) * a.seg
        if not a.no_shrink and err is None:
            small = shrink(a.cpp, a.spec, segs[i])
            os.makedirs(a.save, exist_ok=True)
            path = os.path.join(a.save, f"seed{a.seed}_seg{i}.txt")
            with open(path, "w") as f:
                f.write("\n".join(small) + "\n")
            result["shrunk_to"] = len(small)
            result["saved"] = os.path.relpath(path, ROOT)
            print("minimal failing case:\n  " + "\n  ".join(small))
            print("cpp:\n  " + run(a.cpp, "\n".join(small) + "\n").replace("\n", "\n  "))
            print("spec:\n  " + run(a.spec, "\n".join(small) + "\n").replace("\n", "\n  "))
    if a.json:
        with open(a.json, "w") as f:
            json.dump(result, f)
    print(json.dumps(result))
    sys.exit(1 if result["caught"] else 0)


if __name__ == "__main__":
    main()
