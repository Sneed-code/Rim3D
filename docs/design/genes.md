# Genes and lineages

Code: sim `ColonySim/Genes.h`, `Private/Genes.cpp`. Tests: `Tests/GenesTests.cpp`. Data:
`data/defs/Genes/`, `data/defs/Lineages/`, `Rules` def `GeneRules`, scenario `CrashlandedMixed`.
Task X1. Lineages are this setting's xenotypes (`naming.md`): a lineage is a named set of genes.

## Data

A **Genes** def is one gene. Fields the sim reads:

| Field | What |
| --- | --- |
| `statOffsets` | `{stat: number}`, added to the stat |
| `statFactors` | `{stat: number}`, multiplied into the stat |
| `aptitudes` | `{Skills defName: levels}`, added to the pawn's skill level (Great +8, Poor -4, Awful -8) |
| `visual` | `skinColor` + `skinColorAmount`, `hairColor`, `horns`: what looks (A3) and Unreal draw |
| `ability`, `later` | not read yet: `later` says which task reads it |

A stat is `(base + sum of offsets) * product of factors`, over all of a pawn's genes
(`geneStat`). A **Lineages** def lists `genes`, plus the wiki's `metabolicEfficiency` and
`complexity` for the whole lineage and `geneType` (Endogene or Xenogene, for lineage grafts later).

| Lineage | RimWorld | Genes | Metabolic efficiency | Complexity |
| --- | --- | --- | --- | --- |
| Baseline | baseliner | none | 0 | 0 |
| Emberkin | Impid | 15 | 0 | 11 |
| Frostkin | Yttakin | 12 | -1 | 13 |
| Warbred | Hussar | 14 | +2 | 16 (17 on the Hussar page) |
| Ashborn | Waster | 9 | 0 | 13 |
| Delver | Dirtmole | 9 | 0 | 10 |

Gene names, lineage totals and hunger rates (Frostkin x125%, Warbred x80%) are the RimWorld
wiki's. **Most gene numbers are recalled, not read from the wiki** (the wiki's Genes page was too
long to fetch whole): they are listed under `unverified` in the def and are meant to be tuned.
Checked against the wiki: Robust takes 75% damage, a naked Frostkin is comfortable down to -6 C
(Furskin -17 and Furry tail -5; their split is ours), the Fire spew cooldown of 5 days.
Cosmetic alternatives (the wiki's three skin and hair choices for Emberkin) are one gene each
here. Not built: the drug dependency genes (Warbred, Ashborn: drugs are Stage 3), pollution
stimulus, the Frostkin voice, hulk body and no-hair genes, sleep length, UV and indoor-dweller
effects, abilities (fire spew, animal warcall). Each is in the data with a `later` note.

## What the sim reads now

`PawnGenes` is a component on every humanlike pawn (the ones with `PawnLooks`), made from
`PawnLooks::lineage` by `makeGenes`: in `Sim::giveGenes` when a pawn is made, after `SetLineage`,
and after a save is loaded. **It is not saved**: it follows from the lineage, which is. Pawns of
older saves get theirs from their lineage too (all Baseline, so no change).

`GeneEffects` caches the stats the sim reads each step:

| Stat | Read by |
| --- | --- |
| `moveSpeed` (metres per second, as `stats.moveSpeed` x 30) | `MovementSystem`, fighters' walking |
| `comfyTemperatureMin`, `comfyTemperatureMax` (degrees) | `TemperatureSystem::comfortRange` |
| hunger rate: the lineage's metabolism through `GeneRules.hungerRateByMetabolicEfficiency`, times a `foodRate` factor | `NeedsSystem` |
| `moodOffset` | `MoodSystem::traitMoodOffset` |
| `damageTaken` | `HealthSystem::hitPart` (every hit, before armour of any kind exists) |
| `woundHealing` | natural healing |
| `miningSpeed`, `plantWorkSpeed` | `JobSystem::miningSpeed`, `plantWorkSpeed` |
| `aptitudes` | skill level in `JobSystem`, `BillSystem` and `Construction` |

Every other stat in a gene def (`meleeDamage`, `immunityGainSpeed`, `shootingAccuracy`,
`toxicResistance`, `resonanceSensitivity`, `flammability`, `painFactor`, ...) is summed too and can
be asked for with `geneStat(genes, "name", base)`; the task that adds the rule reads it there.

## Looks

`generateLooks` and `setLineage` take the skin colour from the lineage's last gene with
`visual.skinColor` and the hair colour from the last with `visual.hairColor`
(`lineageGeneWith`); Unreal takes horns the same way (`PawnSubsystem.cpp` `MakeLooks`). The old
`visual` block on the lineage def is gone. Emberkin keep the red skin, dark hair and horns of A3;
Ashborn have slate-gray skin and gray hair, Delver light-gray skin.

## Starting colonists

A scenario can have `lineageWeights` (`{Lineages defName: weight}`). Each colonist except the
protagonist then draws a lineage from it with a stream of its own made from the colonist's seed (the
rest of the sim is untouched; keys in name order). `Crashlanded` has none, so every game started the
old way is unchanged; `CrashlandedMixed` is Crashlanded with weights (baseline 70, Emberkin 6,
Frostkin 6, Warbred 4, Ashborn 6, Delver 8), start it with `simrun --start CrashlandedMixed`.
The protagonist is always baseline.

## Not here

Genes of their own per pawn (grafts, inheritance, `LineageGraft`), a gene editor, and Unreal UI
that lists a pawn's genes (the snapshot carries only the lineage; the inspect panel can read the
def). Changing a pawn's lineage with `SetLineage` swaps its whole gene set.
