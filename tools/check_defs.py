#!/usr/bin/env python3
"""Checks every JSON def file under data/defs.

Each file is a JSON array of defs. The folder a file sits in is its def kind
(Terrain, Things, ...). Checks: valid JSON, every def has a unique defName
within its kind, and every parent exists in the same kind with no cycles.
Exits 1 and lists every problem if anything is wrong.
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent / "data" / "defs"


def main() -> int:
    problems = []
    defs_by_kind = {}
    for path in sorted(ROOT.rglob("*.json")):
        kind = path.relative_to(ROOT).parts[0]
        try:
            entries = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as err:
            problems.append(f"{path}: invalid JSON: {err}")
            continue
        if not isinstance(entries, list):
            problems.append(f"{path}: must be a JSON array of defs")
            continue
        kind_defs = defs_by_kind.setdefault(kind, {})
        for i, entry in enumerate(entries):
            name = entry.get("defName") if isinstance(entry, dict) else None
            if not isinstance(name, str) or not name:
                problems.append(f"{path}: entry {i} has no defName")
                continue
            if name in kind_defs:
                problems.append(f"{path}: duplicate {kind} defName '{name}' (also in {kind_defs[name][0]})")
                continue
            kind_defs[name] = (path, entry)

    for kind, kind_defs in defs_by_kind.items():
        for name, (path, entry) in kind_defs.items():
            seen = {name}
            parent = entry.get("parent")
            while parent is not None:
                if parent not in kind_defs:
                    problems.append(f"{path}: {kind} '{name}' has unknown parent '{parent}'")
                    break
                if parent in seen:
                    problems.append(f"{path}: {kind} '{name}' has a parent cycle through '{parent}'")
                    break
                seen.add(parent)
                parent = kind_defs[parent][1].get("parent")

    for problem in problems:
        print(problem)
    count = sum(len(d) for d in defs_by_kind.values())
    print(f"{count} defs checked, {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
