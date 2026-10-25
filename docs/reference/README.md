# RimWorld reference

Compact summaries of RimWorld's Defs, produced by `tools/rimworld_import.py` (see
`docs/design/rimworld-import.md`). Never commit RimWorld's raw files.

One file per kind (`Things.md`, `Needs.md`...), each a table of the imported defs with their key
numbers, plus `import-report.md`, which lists what matched nothing and what was dropped. These files
are generated: change `tools/rimworld_import_spec.py` and re-run the import instead of editing them.

**Units in every table and in the JSON:** durations are game seconds (one sim step); speeds are
metres per game second; days, hours, years, percentages and per-day rates are as in RimWorld. Work
amounts are game seconds at work speed 1.

_Nothing is generated: D3 was dropped and the starting data is written by hand
(`docs/design/starting-data.md`)._
