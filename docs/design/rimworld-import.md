# RimWorld import

Code: `tools/rimworld_import.py` (the engine) and `tools/rimworld_import_spec.py` (what to import and
how to convert). Tests: `tools/tests/test_rimworld_import.py` with small XML fixtures in
`tools/tests/samples/`. Task D2.

**Not used.** D3 (running it against a RimWorld install) was dropped; the starting data is written by
hand instead (`docs/decisions/0003-hand-written-starting-data.md`, `docs/design/starting-data.md`). The
importer and its tests are kept in case a licensed install is ever available; its unit rules are the
ones the hand-written data follows.

## What it does

```
python tools/rimworld_import.py --rimworld "<RimWorld install folder>"
```

Reads `<install>/Data/Core/Defs/**/*.xml` (more packs with `--packs Core Royalty Ideology ...`, later
ones override earlier ones), merges RimWorld's `Name` / `ParentName` inheritance, keeps the defs
listed in the spec, converts them, and writes:

- `data/defs/<Kind>/<Kind>.rimworld.json`: one flat file per kind. Parents are already merged, so
  there is no `parent` or `abstract`. Every def carries `"source": "Core"` (or the expansion).
- `docs/reference/<Kind>.md`: a compact table per kind, for reading what RimWorld's numbers are.
- `docs/reference/import-report.md`: what matched nothing, which Things were pulled in as
  dependencies (a gun's bullet, a plant's harvest), which fields point at defs that are not imported,
  what was dropped and which numbers were converted.

RimWorld's proper names (peoples, factions, enemies, narrators) are swapped for ours from the
"Applied" tables in `docs/design/naming.md` (`tools/naming.py`): a renamed def gets our `defName` and
`label`, and every reference to it follows. The report lists the renames used. Names in the spec stay
RimWorld's, because they are looked up in RimWorld's files.

Only converted numbers and short labels are written. Descriptions, flavour text, sounds, textures
and the raw XML are dropped and never committed. The output is the same every run.

Other options: `--dry-run` (report only), `--strict` (exit 1 if a listed name matched nothing),
`--find REGEX [--class ThingDef]` (list defNames in the install, for finding the right name),
`--out-defs`, `--out-ref`.

If a hand-written def in the same kind folder has the same `defName` as an imported one, the import
stops with an error and writes nothing. The hand-written starting data now uses most of the same
names, so an import would have to replace it file by file.

## Units

Our day is as long as RimWorld's in game time (60,000 ticks = 86,400 game seconds), so days, hours,
years and per-day or per-hour rates carry over unchanged. Everything else is converted when the
file is written, so the sim only ever sees game seconds (one step) and metres:

| RimWorld | Becomes | Rule | Examples |
| --- | --- | --- | --- |
| Ticks, and work amounts (RimWorld counts work in ticks at work speed 1) | game seconds | x 1.44 (3,600 / 2,500) | `workToBuild`, `workToMake`, `workAmount`, `sowWork`, `harvestWork`; fields ending in `Ticks` are renamed to `...Seconds` |
| Combat timings in real seconds | game seconds | x 30 (`kStepsPerRealSecondAt1x`) | gun `warmupTime`, melee `cooldownTime` |
| Combat timings in ticks | game seconds | ticks / 60 x 30 | `ticksBetweenBurstShots` becomes `secondsBetweenBurstShots` |
| Speeds in cells per real second | metres per game second | / 30 | projectile `speed`, the `moveSpeed` stat |

Combat uses real time rather than the x 1.44 rule because pawns and bullets move in real metres per
second at 1x (`docs/architecture.md`), so a 0.3 s warm-up stays 0.3 real seconds. A test checks the
constants against `Clock.h`. Tuning multipliers (`data/tuning.json`) are applied later, by the def
database, never by the importer.

## Field names

- JSON keeps RimWorld's field names, in the order RimWorld has them after merging.
- `statBases` becomes `stats` and `costList` becomes `costs`; stat names become lower camel case
  (`MaxHitPoints` to `maxHitPoints`). Keys that are def names (`Steel` in `costs`, skills in
  `skillGains`) keep their case.
- `<li Class="CompProperties_Refuelable">` becomes `{"type": "Refuelable", ...}`.
- `(3,1)` becomes `[3, 1]`; `0.3~1.2` (a range) becomes `[0.3, 1.2]`; curves are lists of pairs.
- Backstory `skillGains` is always a dictionary, whichever XML shape the RimWorld version uses.
- A def field called `parent` or `abstract` is renamed `rwParent`, `rwAbstract`, because those words
  mean something to our own defs. `ThingCategoryDef.parent` is `parentCategory`.
- Flag lists such as `foodType` stay as RimWorld's comma-separated text.

## Inheritance rules assumed

Same as RimWorld: a child's fields win, objects merge field by field, a list the child writes
replaces the parent's whole list, `Inherit="False"` drops the parent's version of a field. D3 should
spot-check a few defs with deep inheritance (the stove, the wall) against the real XML.

## Adding something to the import

Open `tools/rimworld_import_spec.py`. Add a name or glob to a `KindSpec`, or add a new `KindSpec`
for a new class (`rw_class` is RimWorld's XML element name). Add a `UNITS` entry for any new
tick-valued field. Run the tests, then `--find` and `--dry-run` on sneed's PC to see what matched.
Expansions: add the pack to `--packs` and add the names; RimWorld's XML patch files (`Patches/`) are
not applied.

## Known gaps

- Names were chosen from memory of RimWorld's Core defs, not from the files. Expect D3 to find a few
  that are spelled differently (the report lists every miss; `--find` finds the right name).
- XML patch operations are not applied.
- References between defs are checked only for the fields in `REFERENCE_FIELDS`, and reported, not
  fixed. `check_defs.py` does not check cross-references yet.
