# Races

sneed's races (2026-11-19). Each race is a lineage ([genes.md](genes.md)) whose genes carry sneed's numbers; the sim
reads them through `GeneEffects` (`ColonySim/Genes.h`). Data: `data/defs/Lineages/Lineages.json`,
`data/defs/Genes/RaceGenes.json`, `Rules/GeneRules.json`. Tests: `Tests/RacesTests.cpp`, `Tests/GenesTests.cpp`,
`Tests/ClimbTests.cpp`.

| Race | Lineage | Good | Bad |
| --- | --- | --- | --- |
| Baseline human | `Baseline` | learns skills 20% faster | |
| Impid | `Emberkin` (label "Impid") | heat, cold and fire do 10% of the harm; no heat injury or heat discomfort until 100 C past a human's; walks 5% faster | takes 5% more damage |
| Orcoid | `Orcoid` | melee blows +10%; hit points and blood pool +10% (every blood-loss stage comes 10% later) | learns 10% slower, works 10% slower |
| Drow | `Drow` | shots +10% damage; dark vision (no slower working or walking in the dark); works 10% faster in the dark (light under 0.3); never feels cooped up indoors (like an undergrounder); nocturnal schedule by default | sunlight sensitivity: outdoors while the sun is up, aim and work 10% worse |
| Cat-strain (beast-strain) | `CatStrain` | jumps twice as high; climbs walls twice as fast ([climbing.md](climbing.md)); 20% less fall and collapse damage | every penalty a third worse (below) |

## How each is read

| Gene stat | Where |
| --- | --- |
| `learningRate` | skill experience (`GatherJobs.cpp`, with the learner traits) |
| `workSpeed`, `darkWorkSpeed`, `sunWorkSpeed` | `JobSystem::workFactor` through `geneWorkFactor` (every work speed: building, crafting, plants, mining, research, doctoring, hunting) |
| `darknessSlowdown` 0 (dark vision) | the light's work and walk slowdown is skipped (`geneLightFactor`) |
| `moveSpeedFactor` | `MovementSystem` |
| `meleeDamage` | `CombatSystem::traitMeleeDamage` (every melee blow); also reads the wiki genes' Strong/Weak melee now |
| `rangedDamage` | `CombatSystem::fire`, the shot's damage factor |
| `shootingAccuracy`, `sunShootingAccuracy` | the aim factor Sim gives Combat (`setAimFactor`); also reads Nearsighted now |
| `healthScale` | `Health::lineageHp`: part and zone hit points, and blood lost a day divided by it |
| `thermalDamage` | hypothermia and heatstroke build-up (`TemperatureSystem::exposePawns`) and burn damage (`Fire` pawn hurt) |
| `comfyTemperatureMax` +100 | the Impid's heat comfort and heat injury start 100 C higher |
| `environmentDamage` | roof collapse and falling-floor damage (`RoofCollapse`) |
| `debuffFactor` | `geneWorsen`: pain, lost capacities, limb slows (health v2), bad mood thoughts, and tiredness and hunger penalties (`NeedsSystem::performanceFactor`) |
| `jumpHeight` | the snapshot's `jumpHeight`, Unreal's jump (higher and a little longer) |
| `climbSpeed` | wall climbs only (`Mover::climbFactor`) |
| `noOutdoorsNeed` | the "cooped up" thought never comes |
| `nocturnal` | `Sim::giveNocturnalSchedule`: the standard schedule turned round by 12 hours when the pawn joins |

"Sunlit" is under open sky (`Lights::underOpenSky`) while the sun's glow is over `GeneRules.sunAboveGlow` (0.3); outdoors
at night a drow gets the dark bonus instead. The "in the dark" mood thought is not switched on for anyone yet (the
Dark thought's `lightBelow` condition has no code); when it is, dark vision should skip it.

## Who is which race

The player's colonist and the starting colonists are always baseline humans (sneed). Joiners (wanderers, refugees) and
raiders roll their race from `GeneRules.joinerLineageWeights` (baseline 60, Impid 10, Orcoid 10, drow 10, cat-strain 10;
ours), with a random stream of their own from their seed so nothing else in the game changes. Traders stay baseline.
`CrashlandedMixed` (a test scenario) still rolls RimWorld's lineages for its starting colonists.

## Drow at night (sneed's question)

A night skip doesn't jump the clock: the sim runs fast, so everyone awake keeps working. It waits only for colonists on a
Sleep hour who are tired, so drow on their night shift never hold it up and work through it as on any night. The
night owl trait is mood only (+8 awake at night, -5 awake by day); the drow default changes only the schedule, so a drow
night owl gets both.

## Looks

Unreal draws the races from the lineage: horns from `MiniHorns`, skin and hair colours from the cosmetic genes
(`DrowSkin`, `PlatinumHair`, `DeepRedSkin`, `EmberHair`). Cat parts (`CatEarsTail`, `visual.catParts`) and the drow's red
irises and long ears are in the creator ([makehuman.md](makehuman.md)); in-game pawns get them when the creator's kinds
are joined to the lineages (later).
