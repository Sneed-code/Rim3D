# 0003: Hand-written starting data instead of the RimWorld import

_Decided 2026-10-25 by sneed._

**What.** Task D3 (running `tools/rimworld_import.py` against a local RimWorld install) is dropped:
there is no licensed copy to read from. Instead the starting defs in `data/defs/` are written by
hand, with RimWorld's numbers taken from public pages such as the RimWorld wiki. Numbers only: no
descriptions, flavour text, textures or sounds, the same rule the importer had. Backstories and
name lists are our own. No local RimWorld files are read.

**Why.** It unblocks C1, C2, C8 and F1 without waiting on a PC task, and sneed sees RimWorld's
numbers as a base for a 3D game that will change a lot, not something to match exactly.

**What it replaces.** The "RimWorld import" box in `architecture.md` and rule 3 of the Stage 1
plan ("numbers come from the RimWorld import"). From now on each task adds the defs it needs,
following `docs/design/starting-data.md`. The importer (D2) and its tests stay in `tools/` but are
not part of the pipeline; `docs/reference/` stays empty.
