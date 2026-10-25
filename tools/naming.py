"""Reads the rename table in docs/design/naming.md: RimWorld defNames to our own names.

The tables there have four columns: RimWorld's name, RimWorld's defName, our name, our defName. A row
counts when both defName cells hold names in backticks; a cell may list several, comma-separated, in
the same order as the names in the "ours" cell. Only rows under the "## Applied" heading are
returned by default. Standard library only.
"""
from __future__ import annotations

import re
from pathlib import Path
from typing import Dict, NamedTuple

NAMING_MD = Path(__file__).resolve().parent.parent / "docs" / "design" / "naming.md"


class Rename(NamedTuple):
    def_name: str   # our defName
    label: str      # our label, as the player reads it


def _cells(line: str):
    return [c.strip() for c in line.strip().strip("|").split("|")]


def _ticked(cell: str):
    return re.findall(r"`([^`]+)`", cell)


def load_renames(path: Path = NAMING_MD, section: str = "Applied") -> Dict[str, Rename]:
    """RimWorld defName -> Rename, for every usable row under the `## <section>` heading."""
    out: Dict[str, Rename] = {}
    in_section = False
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("## "):
            in_section = line[3:].strip().lower().startswith(section.lower())
            continue
        if not in_section or not line.lstrip().startswith("|"):
            continue
        cells = _cells(line)
        if len(cells) != 4:
            continue
        theirs, ours = _ticked(cells[1]), _ticked(cells[3])
        if not theirs or len(theirs) != len(ours):
            continue
        # "Orrin (steady build-up)" -> "Orrin"; "resonance drone, resonance calm" -> two labels.
        label_text = re.sub(r"\s*\([^)]*\)", "", cells[2])
        labels = [x.strip() for x in label_text.split(",")]
        if len(labels) != len(ours):
            labels = [labels[0]] * len(ours)
        for rw, mine, label in zip(theirs, ours, labels):
            if rw != mine:
                out[rw] = Rename(mine, label)
    return out
