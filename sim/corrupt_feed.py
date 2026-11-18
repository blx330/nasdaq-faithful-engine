"""Negative control for the replay checker: inject known faults into a feed.

Copies an ITCH file and inserts exactly three faults, each at a recorded
message number:
  * a duplicate Add of an order that is still live,
  * an Execute for one share more than the order has left,
  * a Delete of a reference that never existed.

A correct checker reports duplicate_ref = overfill = unknown_ref = 1, a
book that is never crossed in continuous trading, and nothing else. A checker that reports less has lost the ability to see that
kind of corruption, which is how two of the seeded bugs were caught.

    python3 sim/corrupt_feed.py data/synth.itch data/corrupt.itch data/corrupt_expected.json
"""
import json
import struct
import sys


def main(src, dst, expected_path):
    data = open(src, "rb").read()
    msgs, off = [], 0
    while off + 2 <= len(data):
        n = struct.unpack_from(">H", data, off)[0]
        msgs.append(data[off + 2: off + 2 + n])
        off += 2 + n

    live = {}
    out, injected = [], {}
    targets = {"dup": len(msgs) * 3 // 10, "over": len(msgs) // 2, "unknown": len(msgs) * 7 // 10}

    def emit(m):
        out.append(struct.pack(">H", len(m)) + m)

    for i, m in enumerate(msgs):
        t = chr(m[0])
        emit(m)
        if t in "AF":
            ref = struct.unpack_from(">Q", m, 11)[0]
            live[ref] = struct.unpack_from(">I", m, 20)[0]
            if "dup" not in injected and i >= targets["dup"]:
                emit(m)  # same Add again: same reference, still live
                injected["dup"] = len(out)
        elif t in "ECX":
            ref, q = struct.unpack_from(">QI", m, 11)
            if ref in live:
                live[ref] -= q
                if live[ref] <= 0:
                    del live[ref]
                elif "over" not in injected and i >= targets["over"] and t == "E":
                    bad = bytearray(m)
                    struct.pack_into(">I", bad, 19, live[ref] + 1)
                    emit(bytes(bad))  # one share more than is left
                    injected["over"] = len(out)
        elif t == "D":
            live.pop(struct.unpack_from(">Q", m, 11)[0], None)
            if "unknown" not in injected and i >= targets["unknown"]:
                bad = bytearray(m)
                struct.pack_into(">Q", bad, 11, 0xFFFFFFFFFFFF)
                emit(bytes(bad))
                injected["unknown"] = len(out)
        elif t == "U":
            old, new, q = struct.unpack_from(">QQI", m, 11)
            live.pop(old, None)
            live[new] = q

    open(dst, "wb").write(b"".join(out))
    exp = {"duplicate_ref": 1, "overfill": 1, "unknown_ref": 1, "crossed_continuous": 0, "injected_at": injected,
           "first_anomaly_msg": min(injected.values())}
    json.dump(exp, open(expected_path, "w"), indent=1)
    print(f"wrote {len(out)} messages with faults at {injected}")


if __name__ == "__main__":
    main(*sys.argv[1:4])
