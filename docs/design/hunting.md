# Hunting and butchering (task N2)

A hunt mark on a wild animal, the Hunting job (walk up, shoot it dead), carrying corpses home, and the butcher table
cutting a corpse up into meat and leather. Code: `HuntJobs.cpp` (the scanners and the three jobs),
`Husbandry.cpp` (the mark and the corpse questions), `Bills.cpp` (the butcher recipe). Tests: `Tests/HuntingTests.cpp`.
Numbers: `data/defs/Rules/HusbandryRules.json` and the `ButcherCorpse` recipe. Builds on
[animal-life.md](animal-life.md) (corpses), [husbandry.md](husbandry.md) (marks) and [crafting.md](crafting.md) (bills).

## The player's side

| Command | What it does |
| --- | --- |
| `MarkAnimal` `Hunt` | A wild, living animal is marked for the hunt. `on` false takes the mark off. A tame animal can't be marked (slaughter it). Marking an animal for the hunt takes its Tame mark off, and the other way round. |
| `AddBill` `ButcherCorpse` | A butcher table cuts corpses up. Count, until-you-have and forever work as for any bill; **until you have counts all raw meat** on the map. The bill's radius limits which corpses it takes (from the table). |

The snapshot's `ThingView::markHunt` shows the mark.

## Hunting

- Who: a colonist with the **Hunting** work type, **a gun in the hands** (a melee weapon is no hunting weapon) and working
  hands. The work type is Violent, so a colonist who can't do violence is never sent. The scanner (`HuntDesignated`) lists marked,
  living, wild animals on the colonist's level, nearest first.
- The `Hunt` job is one long step. The hunter walks at the animal (a new path every `huntRepathSeconds`, 3 s, or when the animal
  has moved three cells) until it is within **80% of the gun's range** (`huntRangeShare`, at least 3 m: ours) with a clear line of sight, then
  stands still and shoots (`CombatSystem::shoot`, an aimed shot: the gun's warm-up and its spread by the hunter's Shooting skill).
  It holds still while aiming and firing a burst, and shoots again whenever the gun is ready. A runner is followed.
- The job ends when the animal is dead: `Husbandry::finishHunt` takes the mark off, sets the corpse's `collect` flag, counts it
  (`stats.hunted`) and sends `AnimalHunted`. It fails (and the colonist keeps away from that animal for ten minutes) when the mark is
  taken off, the gun is gone, no path is found, or **15 minutes (`huntMaxSeconds`, ours) pass without a kill**.
- **Hunted animals turn on the hunter** through N5, not through anything new here: every bullet that hurts an animal calls
  `AnimalSystem::hurt`, which makes tuskers and predators fight back and lets any hurt animal turn manhunter by its chance
  ([animal-fights.md](animal-fights.md)); the colony alarm then calls the colonists up as for any enemy.

## Corpses

A corpse is the dead animal itself (a pawn with `Health::dead`), as for N1. `Husbandry::isCorpse` says an animal is a corpse to cut up: dead,
on the map, and its def has a `race.meatDef` or `race.leatherDef`. `wantsHauling` says a hauler should move it: it has
the `collect` flag (killed by a hunter, or slaughtered) or still has the hunt mark (a marked animal that something else killed).
The 15-day clear-away timer of N1 still runs, in a stockpile as anywhere.

**Hauling** (`HaulCorpses`, in the Hauling work type, after `HaulToStockpile`): a corpse that wants hauling and lies outside every
stockpile is carried to the nearest stockpile cell that takes it (a stockpile takes it unless it disallows the animal's def) and
that holds no item, no other corpse and nobody's reservation. A corpse in a stockpile stays. Nobody hauls without a stockpile.
The job `HaulCorpse` is the rescue job's steps (walk up, lift, carry: the corpse's position follows the carrier) and then
`DropCorpse`, which puts it down in the cell centre (`PawnAction::DropCorpse`). Animals that learned Haul don't carry corpses.

## Butchering

- The recipe `ButcherCorpse` (users: the butcher table; skill Cooking; giver `DoBillsButcher`, in the Cooking work type) has a
  `butcher` field; `Recipe::butcher` is set, and the bill giver makes a `Butcher` job instead of fetching item stacks.
- **Which corpse:** the nearest to the table of the dead animals that `isCorpse`, that the bill's radius allows (0: anywhere), that
  nobody else holds, that the colonist hasn't just failed on and can reach. Any dead animal will do, not only hunted ones.
  Human corpses can't be butchered yet.
- **The job** (`Butcher`): walk to the corpse, lift it, carry it to the cell beside the table, work, finish. The work is
  **648 seconds** (RimWorld: 450 ticks, wiki; 1.44 seconds a tick), done at the colonist's Manipulation times the light.
  *Not* by body size and not by Cooking skill: skill decides the yield.
- **The yield** is `race.meatAmount` and `race.leatherAmount` of the animal's def, times its body size against an adult's
  (a young animal gives less), times the **butchery efficiency** = (75% + 2.5% per Cooking level) x (1 + 0.9 x (Manipulation - 1)),
  at most 150% (the wiki's formula; the table has no penalty). Cooking 8 with healthy hands: 95%. A deer (90 meat, 50 leather):
  86 and 48 from an adult. The colonist learns Cooking (30 xp).
- The corpse is destroyed and the meat and leather are dropped by the colonist (haulers carry them off). Events: `AnimalButchered`
  (`thing` the butcher, `def` the animal, `target` the table, `amount` the meat, `part` the leather) and `RecipeDone`;
  `PawnAction::Butcher` when the work starts.

## Saves, hash, replay

Section `husbandry` is **version 2**: `markHunt` and `collect` per animal care record and `hunted` in the counters (version 1
files load with none). The state hash adds them only when set, so older recordings hash as before. The job check accepts
`Hunt`, `HaulCorpse` and `Butcher`. The command is `MarkAnimal` with mark `Hunt` (nothing new in the replay format).

## Not done

- No skill experience for hunting itself (Shooting xp for the shots is up to the combat system), no hunting with melee weapons.
- The butcher spot (70% yield) and tool cabinets next to the table, the -6 mood for butchering people, human corpses, damaged
  corpses giving less (the wiki says 66%, marked "fact check") and rotting corpses.
- A bill's "store in this stockpile" doesn't move meat and leather: they are carried like any item.
- A hunted animal that drifts off the map edge stays marked; the hunter gives up after 15 minutes.
- Hunters don't weigh the danger: a marked bear gets shot at like a marked hare (BACKLOG: skip dangerous animals).
