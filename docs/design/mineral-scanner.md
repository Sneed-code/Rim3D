# Long-range mineral scanner

The `LongRangeMineralScanner` project (2,000, after Microelectronics and Machining) unlocks the **long-range
mineral scanner** (`Things/BuildingsResearch.json`): 3 x 3, 700 W, 200 steel + 6 components, 250 HP, beauty -8
(wiki numbers, checked 2026-11-13; the wiki's 2 advanced components are left out, we have none). Like the
ground-penetrating scanner (`deep-mining.md`) it is worked in the Research work type (Intellectual) by a colonist
beside it, and does nothing under a roof or without power. It reuses the `ScanDeep` work giver, the `DeepWork` job
and the operator's research speed.

## A find

A find is due on average every 240,000 ticks of work (345,600 s) and always by 480,000 ticks (691,200 s), scaled
by the operator's speed (`DeepResources::scanDue`, same exponential rule as the deep scanner; a find resets the
work). A find puts a **lump of the tuned ore on the surface**, 6 to 25 cells from the scanner, on free ground (no
building), as mineable ore cells like a vein (`Mineables::place`, the ground becomes rock): the ore's `lumpSize`
cells, each giving its `dropCount`. RimWorld makes a quest for a lump on another map, guarded or ambushed; ours is on
the colony's own map and unguarded for now. `OreLumpFound` (`def` the ore, `cell`, `count`) is raised.

## Tuning

`ScannerCommand{building, ore}` tunes the scanner to an ore in its def's `deepWork.ores`: Ore_Steel, Ore_Plasteel,
Ore_Components, Ore_Silver, Ore_Gold, Ore_Uranium, Ore_Jade. An untuned scanner takes the first (steel). The tuning
survives finds (`DeepWork::tune`, kept by `forgetWork`), is hashed, and saved in section `deep` version 2 (older saves
load untuned).

The four new ores (`Ore_Plasteel`, `Ore_Gold`, `Ore_Uranium`, `Ore_Jade`) have commonality 0: map creation makes none
and draws no random numbers for them, so old maps are unchanged. Gold's lump (8 to 12 cells x 40) matches the wiki's
320 to 480 per lump; plasteel, uranium and jade are marked unverified.

## Not yet

A tune picker in the building's panel (Unreal), guarded lumps (manhunter packs, ambushes), the lump expiring after 30
days, steel lumps at the wiki's 2,200 to 2,400 (ours are 20 to 40 cells x 40 = 800 to 1,600). Tests: `MultiAnalyzerTests.cpp`.
