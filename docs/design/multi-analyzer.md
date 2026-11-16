# Multi-analyzer

Part of the research tree (`docs/design/research.md`). The `MultiAnalyzer` project unlocks the **multi-analyzer**
(`Things/BuildingsResearch.json`): 2 x 2, 200 W, 40 steel + 50 plasteel + 20 gold + 8 components, 200 HP, 10,000
ticks of work (wiki numbers, checked 2026-11-13; the wiki's Construction skill 8 is not modelled).

## What it does

- **Link.** A powered analyzer links every research bench whose middle is within `building.facilityRange` (8 cells)
  of its own middle, on the same level (`JobSystem::benchLinkedTo`). A building under construction, marked for
  deconstruction or without power links nothing. Several analyzers do not add up.
- **Speed.** A linked bench with `researchBench.multiAnalyzerBonus` (the hi-tech bench, 0.1) works at `speedFactor +
  bonus`, so 1.1. The simple bench has no bonus and does not link.
- **Required facility.** The 11 projects with `requiredFacility: "MultiAnalyzer"` (Fabrication, Bionic replacements,
  Vitals monitor, Advanced fabrication, the uranium slug turret, the rocketswarm launcher and the four spacer
  armour and weapon projects) can only be worked at a linked bench: the work giver does not offer an unlinked bench for
  them and a job at a bench that lost its analyzer ends. Before this the field was ignored.

## Not yet

Drawing the link (a line from the analyzer to its benches), a research window hint "needs a multi-analyzer", and
minifying the analyzer. Tests: `MultiAnalyzerTests.cpp`.
