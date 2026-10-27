# Naming: our names for RimWorld's

_Started 2026-10-25. The setting is in [lore.md](lore.md)._

Rim3D plays like RimWorld but has its own setting, so RimWorld's **proper names and made-up words**
get our own names. **Plain words stay as they are**: steel, wood, silver, component, medicine, wall,
door, bed, rifle, sniper rifle, revolver, flak vest, cut, gunshot, shooting, mining, traits like
tough or night owl, and pirates. RimWorld's mechanics and numbers are not affected.

**Where names live.** Names the player sees live in data files (`label` in `data/defs/`), so a
rename is a data change. `defName`s use our names too, so code and data read the same way.
This page is also read by machines: the RimWorld importer (`tools/rimworld_import.py`, via
`tools/naming.py`) reads every table row under "Applied" that has a RimWorld defName and our defName,
and renames those defs (and every reference to them) in what it writes, so imported data comes out
with our names. Keep the four-column table shape; `tools/tests/test_naming.py` checks it parses.

**Adding a name.** Pick a name that fits [lore.md](lore.md), add a row here, then use it in data and
code. Old decision records in `docs/decisions/` keep the
names they were written with.

## Applied: the setting's names

These are the names of the story: peoples, enemies, factions, places, narrators and the words for
RimWorld's lore systems. `RimWorld defName` is the name in RimWorld's files where there is one.

### Peoples (RimWorld's xenotypes, here "lineages")

| RimWorld | RimWorld defName | Ours | Our defName |
| --- | --- | --- | --- |
| xenotype | | lineage | |
| baseliner | `Baseliner` | baseline | `Baseline` |
| Impid | `Impid` | Emberkin | `Emberkin` |
| Sanguophage | `Sanguophage` | Vesper | `Vesper` |
| Hussar | `Hussar` | Warbred | `Warbred` |
| Yttakin | `Yttakin` | Frostkin | `Frostkin` |
| Waster | `Waster` | Ashborn | `Ashborn` |
| Dirtmole | `Dirtmole` | Delver | `Delver` |
| Genie | `Genie` | Savant | `Savant` |
| Highmate | `Highmate` | Charmer | `Charmer` |
| Pigskin | `Pigskin` | Boarkin | `Boarkin` |
| Neanderthal | `Neanderthal` | Neanderthal (a real word, kept) | `Neanderthal` |
| xenogerm | `Xenogerm` | lineage graft | `LineageGraft` |

### Enemies and creatures of the setting

| RimWorld | RimWorld defName | Ours | Our defName |
| --- | --- | --- | --- |
| mechanoids, mechs | `Mechanoid` (faction) | the Remnant, remnants | `Remnant` |
| Scyther | `Mech_Scyther` | remnant reaper | `Remnant_Reaper` |
| Lancer | `Mech_Lancer` | remnant striker | `Remnant_Striker` |
| Pikeman | `Mech_Pikeman` | remnant ranger | `Remnant_Ranger` |
| Centipede | `Mech_Centipede` | remnant crawler | `Remnant_Crawler` |
| Termite | `Mech_Termite` | remnant borer | `Remnant_Borer` |
| mechanitor | | binder | |
| mechlink | `MechlinkImplant` | binder link | `BinderLink` |
| insectoids | `Insect` (faction) | chitinids | `Chitinid` |
| Megascarab | `Megascarab` | chitinid drone | `Chitinid_Drone` |
| Spelopede | `Spelopede` | chitinid stalker | `Chitinid_Stalker` |
| Megaspider | `Megaspider` | chitinid warrior | `Chitinid_Warrior` |
| insect hive | `Hive` | brood mound | `BroodMound` |
| void monolith, Anomaly entities | `VoidMonolith` | the Black Spire | `BlackSpire` |

### Factions

| RimWorld | RimWorld defName | Ours | Our defName |
| --- | --- | --- | --- |
| Outlander Union (civil outlanders) | `OutlanderCivil` | Fray Compact | `FrayCompact` |
| rough outlanders | `OutlanderRough` | free hold | `FreeHold` |
| gentle tribe | `TribeCivil` | gentle clan | `ClanGentle` |
| fierce tribe | `TribeRough` | fierce clan | `ClanFierce` |
| savage tribe | `TribeSavage` | savage clan | `ClanSavage` |
| the Empire | `Empire` | the Concord | `Concord` |
| ancients (cryptosleep survivors) | `Ancients` | old crews | `OldCrews` |
| pirates | `Pirate` | pirates (kept) | `Pirate` |

### Places, powers and lore words

| RimWorld | RimWorld defName | Ours | Our defName |
| --- | --- | --- | --- |
| rimworld (a frontier planet) | | fray world | |
| glitterworld | | lumen world | |
| urbworld | | sprawl world | |
| midworld, medieval world, animal world | | fallen world | |
| archotechs | | Sleepers | |
| AI persona core | `AIPersonaCore` | Sleeper shard | `SleeperShard` |
| glitterworld medicine | `MedicineUltratech` | lumen medicine | `MedicineLumen` |
| psycasts, psychic powers | | resonance | |
| psylink, psycaster | `PsychicAmplifier` | attunement, attuned | `Attunement` |
| psychic drone, psychic soothe | `PsychicDrone`, `PsychicSoothe` | resonance drone, resonance calm | `ResonanceDrone`, `ResonanceCalm` |
| psycast (one power) | | resonance power, power | |
| neural heat | | strain | |
| psyfocus | | focus | |
| psychic sensitivity | `PsychicSensitivity` | resonance sensitivity | `ResonanceSensitivity` |
| Painblock (psycast) | `Painblock` | numb | `Numb` |
| Skip (psycast) | `Skip` | jump | `Jump` |
| ideoligion | | creed | |
| gravship | | skyhull | |

### Narrators (RimWorld's storytellers)

| RimWorld | RimWorld defName | Ours | Our defName |
| --- | --- | --- | --- |
| storyteller | | narrator | |
| Cassandra Classic | `Cassandra` | Orrin (steady build-up) | `Orrin` |
| Phoebe Chillax | `Phoebe` | Maren (long quiet spells) | `Maren` |
| Randy Random | `Randy` | Jinx (anything goes) | `Jinx` |

## Proposed: made-up item and creature words

Not applied yet. These are RimWorld's invented words for items, drugs, fabrics and animals. They stay
as RimWorld has them until sneed decides; the importer ignores this section.

| RimWorld | RimWorld defName | Ours | Our defName |
| --- | --- | --- | --- |
| crashlanded (start) | `Crashlanded` | pods down | `PodsDown` |
| plasteel | `Plasteel` | vantium | `Vantium` |
| packaged survival meal | `MealSurvivalPack` | ration pack | `MealRationPack` |
| nutrient paste meal | `MealNutrientPaste` | protein paste | `MealProteinPaste` |
| luciferium | `Luciferium` | lazarine | `Lazarine` |
| go-juice | `GoJuice` | rush | `Rush` |
| yayo | `Yayo` | dust | `Dust` |
| smokeleaf | `SmokeleafLeaves` | dreamleaf | `DreamleafLeaves` |
| psychite | `PsychoidLeaves` | hazeroot | `HazerootLeaves` |
| devilstrand | `DevilstrandCloth` | ironsilk | `IronsilkCloth` |
| hyperweave | `Hyperweave` | lumenweave | `Lumenweave` |
| muffalo | `Muffalo` | shagback | `Shagback` |
| boomalope | `Boomalope` | blastbuck | `Blastbuck` |
| boomrat | `Boomrat` | popper rat | `PopperRat` |
| thrumbo | `Thrumbo` | vellhorn | `Vellhorn` |
| alphabeaver | `Alphabeaver` | gnawer | `Gnawer` |
