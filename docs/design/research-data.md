# Research data

Task D12. Data only: `data/defs/Research/Research.json`, `data/defs/TechLevels/TechLevels.json`,
`data/defs/Things/ResearchBenches.json`. C21 (research jobs and gating) reads it.

## Projects (`Research`)

71 projects (vanilla RimWorld's base-game tree, plus our MarineArmor, ReconArmor, PulseChargedMunitions and CataphractArmor; other DLC projects are not in), with RimWorld wiki costs (research points): `baseCost`, `techLevel`, `prerequisites`
(shown in the tree), `hiddenPrerequisites` (also required, as in RimWorld), `benchTier` (1 = any
research bench, 2 = hi-tech bench only) and `unlocks`, a list of defNames per kind (`Things`,
`Recipes`, `Terrain`). `requiredFacility` ("MultiAnalyzer") marks projects that also need a
multi-analyzer next to the hi-tech bench; the facility itself is not defined yet.

| Level | Projects |
| --- | --- |
| Neolithic | PassiveCooler 400, Devilstrand 800 |
| Medieval | ComplexClothing 600, ComplexFurniture 300, CarpetMaking 800, Smithing 700, Stonecutting 300, LongBlades 400 |
| Industrial | DrugProduction 500, Electricity 1600, Battery 400, SolarPanel 600, AirConditioning 500, SterileMaterials 600, Machining 1000, Prosthetics 600, Microelectronics 3000, HospitalBed 1200, MedicineProduction 1500 |
| Spacer | MultiAnalyzer 4000, Fabrication 4000, BionicReplacements 3000 |

Names are RimWorld's plain words. Bionic replacements' cost and prerequisites, the Spacer level of
the last three, and CarpetMaking's missing prerequisite were not on the wiki pages read and are
listed in each project's `unverified`. Electricity's exact unlock list (wind turbine, sun lamp,
electric stove) is also recalled, not read.

`TechLevels` has `order` and `costFactorForNeolithicStart`: a colony that starts at neolithic pays
1.5x for medieval projects and 2x for industrial and above (the Crashlanded start pays 1x).

## Gating content

A def (Thing, Recipe or Terrain) that a project unlocks carries `researchPrerequisite`, a project
name or a list (all needed); children inherit it from the parent. `tools/check_defs.py` checks both
directions: every named project exists and lists the def in `unlocks`, and every `unlocks` entry
names the project back. D6 already used this field for ComplexClothing and Electricity.

Not gated, because RimWorld starts with them or they are neolithic: bed, tables, stool, stoves
(fueled), campfire, torch lamp, crafting spot, plant pot, column, knife, club, peg leg and wooden
parts. Which projects a new colony starts with done is C21's decision (the scenario file).

## Research benches (`Things/ResearchBenches.json`)

`SimpleResearchBench` (3x2, speed factor 0.75, 75 stuff + 25 steel) and `HiTechResearchBench` (5x2,
speed 1.0, +0.1 with a multi-analyzer, 250 W, needs Microelectronics). `researchBench.tier` matches
a project's `benchTier`: a bench works on projects of its tier or lower. Work to build is the
wiki's ticks x 1.44.

The research system that reads all this is [research.md](research.md) (C21).

The tree was completed against the wiki's research page. Projects that need Microelectronics (directly or through a prerequisite) are `benchTier` 2; those that need the Multi-analyzer project also carry `requiredFacility`. Projects whose unlocks are not in the game yet have an empty `unlocks` (listed in `BACKLOG.md`); guns, flak armor, the simple helmet, breach axe, IED and turrets are linked to their projects.
