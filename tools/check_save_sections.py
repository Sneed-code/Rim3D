#!/usr/bin/env python3
"""Fails if Save.cpp names the same save section twice (a bad merge once left two 'raids' sections, and no save would load)."""
import collections
import pathlib
import re
import sys

source = pathlib.Path(__file__).resolve().parent.parent / "game/Plugins/ColonySim/Source/ColonySim/Private/Save.cpp"
names = re.findall(r'ar\.section\("(\w+)"', source.read_text(encoding="utf-8"))
dupes = sorted(n for n, c in collections.Counter(names).items() if c > 1)
if dupes:
    print("save sections written twice:", ", ".join(dupes))
    sys.exit(1)
print(f"{len(names)} save sections, none twice")
