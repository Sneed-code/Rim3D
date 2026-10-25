#!/usr/bin/env python3
"""Compares the state hashes of two recorded replays of the same session.

  python3 tools/compare_replays.py linux.replay.json windows.replay.json

Both must have the same settings and commands. Prints the first game second where the state
hashes differ and exits 1, or says they match. CI uses it to check that the sim gives the same
game on Linux and Windows (docs/design/bridge.md).
"""
import json
import sys


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def hashes(replay):
    out = {c["step"]: c["hash"] for c in replay.get("checkpoints", [])}
    if "endHash" in replay:
        out[replay["endStep"]] = replay["endHash"]
    return out


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip())
        return 2
    a, b = load(sys.argv[1]), load(sys.argv[2])
    for key in ("settings", "commands", "defsHash"):
        if a.get(key) != b.get(key):
            print(f"not the same session: '{key}' differs")
            return 1
    ha, hb = hashes(a), hashes(b)
    common = sorted(set(ha) & set(hb))
    if not common:
        print("no state hashes in common to compare")
        return 1
    for step in common:
        if ha[step] != hb[step]:
            print(f"MISMATCH: the state first differs at game second {step} "
                  f"({ha[step]} vs {hb[step]}), {len(common)} hashes compared")
            return 1
    print(f"match: {len(common)} state hashes compared, up to game second {common[-1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
