"""Run every seeded bug against the three validators and build the matrix.

For each mutants/NN_name.patch: copy the sources to a scratch directory,
apply the patch, build, then run
  * fuzzer     - differential fuzzing against the OCaml spec (fuzz/difftest.py)
  * properties - C++ property tests with invariants after every operation
  * replay     - strict replay and fidelity on the ITCH file, the inferred
                 aggressors scored against ground truth, and the replay
                 checker run on a feed with injected faults
and record whether each caught the bug and after how many operations.

    python3 mutants/run_mutants.py --itch data/synth.itch --truth data/truth.jsonl \
        --corrupt data/corrupt.itch --corrupt-expected data/corrupt_expected.json [--ops 200000]

Writes results/mutants.json and results/mutant_matrix.md.
"""
import argparse
import concurrent.futures as cf
import glob
import json
import os
import shutil
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TARGETS = ["nfe_engine", "property_tests", "nfe_replay", "nfe_fidelity"]
COPY = ["CMakeLists.txt", "include", "src", "tests", "fuzz"]


def sh(cmd, cwd=None, timeout=900):
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout)


def build(tree):
    b = os.path.join(tree, "build")
    r = sh(["cmake", "-S", tree, "-B", b, "-DCMAKE_BUILD_TYPE=Release"])
    if r.returncode:
        return None, r.stderr
    r = sh(["cmake", "--build", b, "-j2", "--target"] + TARGETS)
    return (b, None) if r.returncode == 0 else (None, r.stdout[-2000:] + r.stderr[-2000:])


def run_replay(bdir, args, golden, outdir):
    """The replay validator, three checks on ITCH input:
      1. strict replay and fidelity on the clean feed: no anomalies, no
         unexplained events, and the same class counts as the unmutated engine;
      2. the inferred aggressors scored against ground truth, unchanged;
      3. the corrupted feed (negative control): exactly the injected faults."""
    itch = args.itch
    r1 = sh([os.path.join(bdir, "nfe_replay"), itch, "--out", outdir, "--strict"])
    r2 = sh([os.path.join(bdir, "nfe_fidelity"), itch, "--out", outdir])
    msgs = golden["fidelity"]["messages"]
    if r1.returncode < 0 or r2.returncode < 0 or not os.path.exists(os.path.join(outdir, "fidelity.json")):
        return True, None, "crash"
    rep = json.load(open(os.path.join(outdir, "replay.json")))
    fid = json.load(open(os.path.join(outdir, "fidelity.json")))
    first = [x for x in (rep.get("first_anomaly_msg", 0), fid.get("first_unexplained_msg", 0)) if x]
    if r1.returncode != 0 or fid["by_class"]["unexplained"] > 0:
        return True, min(first) if first else msgs, "anomaly on clean feed"
    if fid["by_class"] != golden["fidelity"]["by_class"]:
        return True, msgs, "class counts changed"
    score = os.path.join(outdir, "score.json")
    sh(["python3", os.path.join(ROOT, "analysis", "score_grouping.py"), os.path.join(outdir, "events.csv"),
        args.truth, "--json", score])
    if not os.path.exists(score) or json.load(open(score)) != golden["score"]:
        return True, msgs, "ground-truth score changed"
    cdir = os.path.join(outdir, "corrupt")
    r3 = sh([os.path.join(bdir, "nfe_replay"), args.corrupt, "--out", cdir])
    if r3.returncode < 0:
        return True, None, "crash on corrupted feed"
    rc = json.load(open(os.path.join(cdir, "replay.json")))
    exp = json.load(open(args.corrupt_expected))
    for k in ("duplicate_ref", "overfill", "unknown_ref"):
        if rc[k] != exp[k]:
            missed = {"duplicate_ref": "dup", "overfill": "over", "unknown_ref": "unknown"}[k]
            return True, exp["injected_at"][missed], f"missed injected {k}"
    if rc["crossed_continuous"] != exp["crossed_continuous"]:
        return True, exp["injected_at"]["over"], "book corrupted by an injected fault"
    return False, None, ""


def run_one(patch, args, golden):
    name = os.path.basename(patch)[:-6]
    desc = open(patch).readline().lstrip("# ").strip()
    tmp = tempfile.mkdtemp(prefix="mut_")
    try:
        for c in COPY:
            src = os.path.join(ROOT, c)
            (shutil.copytree if os.path.isdir(src) else shutil.copy)(src, os.path.join(tmp, c))
        r = sh(["patch", "-p1", "-i", patch], cwd=tmp)
        if r.returncode:
            return {"mutant": name, "desc": desc, "error": "patch failed: " + r.stdout + r.stderr}
        bdir, err = build(tmp)
        if not bdir:
            return {"mutant": name, "desc": desc, "error": "build failed", "log": err}
        res = {"mutant": name, "desc": desc}
        # fuzzer
        r = sh(["python3", os.path.join(ROOT, "fuzz", "difftest.py"), "--ops", str(args.ops), "--seed", "11",
                "--cpp", os.path.join(bdir, "nfe_engine"), "--no-shrink",
                "--save", os.path.join(tmp, "regressions"), "--json", os.path.join(tmp, "fz.json")])
        fz = json.load(open(os.path.join(tmp, "fz.json"))) if os.path.exists(os.path.join(tmp, "fz.json")) else {}
        res["fuzzer"] = {"caught": bool(fz.get("caught", r.returncode != 0)), "ops": fz.get("ops_to_detection")}
        # properties
        r = sh([os.path.join(bdir, "property_tests"), str(args.ops), "11"])
        ops = None
        for line in r.stdout.splitlines():
            if line.startswith("ops_to_detection="):
                ops = int(line.split("=")[1])
        res["properties"] = {"caught": r.returncode != 0, "ops": ops if ops else (None if r.returncode == 0 else 0)}
        # replay
        caught, ops, why = run_replay(bdir, args, golden, os.path.join(tmp, "out"))
        res["replay"] = {"caught": caught, "ops": ops, "why": why}
        return res
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def fmt(v):
    if not v["caught"]:
        return "·"
    return f"✓ {v['ops']:,}" if v.get("ops") else "✓"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--itch", required=True, help="synthetic ITCH file")
    ap.add_argument("--truth", required=True, help="its ground-truth JSONL")
    ap.add_argument("--corrupt", required=True, help="the same feed with injected faults")
    ap.add_argument("--corrupt-expected", required=True)
    ap.add_argument("--ops", type=int, default=200_000)
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--out", default=os.path.join(ROOT, "results"))
    ap.add_argument("--build", default=os.path.join(ROOT, "build"))
    args = ap.parse_args()
    for k in ("itch", "truth", "corrupt", "corrupt_expected"):
        setattr(args, k, os.path.abspath(getattr(args, k)))
    os.makedirs(args.out, exist_ok=True)

    # Golden numbers from the unmutated engine.
    gdir = os.path.join(args.out, "golden")
    sh([os.path.join(args.build, "nfe_fidelity"), args.itch, "--out", gdir])
    sh(["python3", os.path.join(ROOT, "analysis", "score_grouping.py"), os.path.join(gdir, "events.csv"),
        args.truth, "--json", os.path.join(gdir, "score.json")])
    golden = {"fidelity": json.load(open(os.path.join(gdir, "fidelity.json"))),
              "score": json.load(open(os.path.join(gdir, "score.json")))}

    patches = sorted(glob.glob(os.path.join(HERE, "*.patch")))
    with cf.ThreadPoolExecutor(args.jobs) as ex:
        results = list(ex.map(lambda p: run_one(p, args, golden), patches))
    json.dump(results, open(os.path.join(args.out, "mutants.json"), "w"), indent=1)

    rows = ["| Mutant | Bug | Fuzzer | Properties | Replay | Caught |", "| --- | --- | --- | --- | --- | --- |"]
    caught_all, only_one = 0, 0
    for r in results:
        if "error" in r:
            rows.append(f"| {r['mutant']} | {r['desc']} | build error | | | ✗ |")
            continue
        hits = [r[v]["caught"] for v in ("fuzzer", "properties", "replay")]
        caught_all += any(hits)
        only_one += sum(hits) == 1
        rows.append(f"| {r['mutant']} | {r['desc']} | {fmt(r['fuzzer'])} | {fmt(r['properties'])} | "
                    f"{fmt(r['replay'])} | {'✓' if any(hits) else '✗'} |")
    per = {v: sum(1 for r in results if "error" not in r and r[v]["caught"]) for v in ("fuzzer", "properties", "replay")}
    med = {}
    for v in ("fuzzer", "properties"):
        xs = sorted(r[v]["ops"] for r in results if "error" not in r and r[v]["caught"] and r[v]["ops"])
        med[v] = xs[len(xs) // 2] if xs else None
    summary = (f"\n**{caught_all} of {len(results)} seeded bugs caught.** "
               f"Fuzzer {per['fuzzer']}, properties {per['properties']}, replay {per['replay']}; "
               f"{only_one} caught by exactly one validator. Median operations to detection: "
               f"fuzzer {med['fuzzer']}, properties {med['properties']}. Cells show operations "
               f"(or ITCH messages, for replay) until the first failure; · means not caught.\n")
    md = "\n".join(rows) + "\n" + summary
    open(os.path.join(args.out, "mutant_matrix.md"), "w").write(md)
    json.dump({"total": len(results), "caught": caught_all, "per_validator": per, "only_one": only_one,
               "median_ops": med}, open(os.path.join(args.out, "mutants_summary.json"), "w"), indent=1)
    print(md)


if __name__ == "__main__":
    main()
