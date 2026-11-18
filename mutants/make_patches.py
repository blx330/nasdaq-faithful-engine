"""Write one unified-diff patch per mutant in definitions.py.

    python3 mutants/make_patches.py
"""
import difflib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from definitions import MUTANTS  # noqa: E402


def main():
    for f in os.listdir(HERE):
        if f.endswith(".patch"):
            os.remove(os.path.join(HERE, f))
    for i, m in enumerate(MUTANTS, 1):
        src = open(os.path.join(ROOT, m["file"])).read()
        n = src.count(m["old"])
        if n != 1:
            sys.exit(f"mutant {m['name']}: pattern found {n} times in {m['file']}")
        mut = src.replace(m["old"], m["new"])
        diff = difflib.unified_diff(src.splitlines(True), mut.splitlines(True),
                                    fromfile="a/" + m["file"], tofile="b/" + m["file"])
        with open(os.path.join(HERE, f"{i:02d}_{m['name']}.patch"), "w") as f:
            f.write(f"# {m['desc']}\n")
            f.writelines(diff)
    print(f"wrote {len(MUTANTS)} patches")


if __name__ == "__main__":
    main()
