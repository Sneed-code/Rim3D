# Repair and weapon upgrades

Code: `ColonySim/Repair.h`, `Private/Repair.cpp` (the marks, the costs, the parts), `Private/GearJobs.cpp` (the
`RepairGear` job), `RepairGearGiver` in `Private/WorkGivers.cpp`, the gun numbers in `Private/Combat.cpp`
(`combatGunOf`) and `Private/Fighters.cpp` (`weaponRange`). Data: `data/defs/Rules/RepairRules.json`,
`Things/WeaponParts.json`, `Recipes/WeaponParts.json`, `repairs` and `fitsUpgrades` on benches. Tests:
`Tests/RepairTests.cpp`. Task P4. It stands on crafting ([crafting.md](crafting.md)), clothes
([apparel.md](apparel.md)) and guns ([combat.md](combat.md), [weapons.md](weapons.md)).

RimWorld cannot repair gear, so **every number here is ours**. Two things happen at a bench: a damaged piece of apparel or a
weapon is mended, and an upgrade part is fitted to a gun.

## Repair

`RepairGearCommand{thing, repair}` marks a piece of apparel or a weapon (gun or melee) that **lies on the map**
(on the ground or in a stockpile) and has lost hit points. A worn or carried piece has to be taken off and put down first;
the mark goes by itself when the piece is at full hit points (checked every game minute).

**Where.** A bench def lists the kinds of gear it repairs in `repairs`: `["Weapons"]` on the fueled and electric
smithy and the machining table, `["Apparel"]` on the hand and electric tailor bench. Two work givers carry the work:
`RepairWeapons` under **Smithing** and `RepairApparel` under **Tailoring**, after the bench bills in each work type.
A colonist with that work type takes the nearest marked piece, carries it to a cell beside a bench that repairs it
(the job `RepairGear`: go to it, pick it up, carry it, work), mends it, and puts it down by the bench. The bench must
run (power) but burns no fuel. One colonist at a time per bench.

**Cost** (`RepairSystem::costOf`, numbers in `RepairRules`). Mending the *whole* loss of an item takes:

| | Work (game seconds at speed 1) | Material |
| --- | --- | --- |
| item with `stats.workToMake` and `costStuffCount` (clothes, melee weapons) | `workToMake` x 0.25 | `costStuffCount` x 0.25 of the stuff it is made of |
| anything else (guns, flak vest, helmets) | 300 per kg of mass | 3 per kg of Steel (weapons) or Cloth (apparel) |

**Apparel is cheaper since 2026-10-27** (sneed): a tenth instead of a quarter, rounded down, 1 cloth and 100 work per kg for apparel with no stuff, and the share lost is the bigger of lost hit points and lost durability; one repair brings both back ([apparel-durability.md](apparel-durability.md)). The table and the vest example below are the weapon numbers and the old apparel ones.

A part-damaged item takes the share it lacks (`(max - current) / max`), the work rounded up, the material rounded up to
at least 1. A flak vest (4 kg) at half hit points: 600 s of work and 6 cloth. A rifle (3.5 kg) at 40 of 100: 630 s and 7 steel.

**Material.** It is taken, when the work ends, from the stacks nearest the bench **within 12 m** (`materialRadius`);
the colonist does not fetch it. The giver only offers the job when enough lies there, the job fails (nothing used up, the
item put down) if it is taken away meanwhile, and an interrupted job costs nothing. Ours: simpler than a bill's ingredient
fetching, and a stockpile next to the bench does the same.

**Speed.** Each second the work done is `(0.6 + 0.05 x Crafting level)` (the bill system's skill speed), times the
Manipulation capacity, the light at the bench and the bench's `workTableWorkSpeedFactor`. Anyone may repair
(`RepairRules.minSkill` 0). The item comes out at full hit points; quality does not change. Skill experience is not given (as
for bills).

## Upgrade parts

An upgrade part is an item def with an `upgrade` field:

```json
"upgrade": {"slot": "Optic", "weaponSlots": ["Primary"], "work": 900, "minSkill": 6,
            "stats": {"rangeFactor": 1.25, "accuracyFactor": 1.05, "warmupFactor": 1.1}}
```

A gun takes **one part per slot** (Optic, Magazine, Barrel, Muzzle, Trigger). `weaponSlots` says whether it fits rifles
(`Primary`), sidearms or both. The five parts (all numbers ours; made at the machining table, research Machining, Steel and
industrial components, `Make_WeaponPart_*`):

| Part | Slot | Fits | Changes | Fitting work, skill |
| --- | --- | --- | --- | --- |
| scope | Optic | rifles | range x1.25, accuracy x1.05, warm-up x1.1 (slower to aim) | 900, 6 |
| extended magazine | Magazine | all | magazine x1.5 (at least one more round), reload x1.2 | 600, 4 |
| long barrel | Barrel | all | range x1.15, accuracy x1.05 | 700, 5 |
| recoil compensator | Muzzle | all | recoil x0.7 (shot spread bloom and the player's gun kick) | 500, 5 |
| hair trigger | Trigger | all | cooldown x0.85, warm-up x0.85, accuracy x0.97 | 500, 6 |

`FitUpgradeCommand{thing, part, fit}` orders a part (a Things defName) fitted to a gun lying on the map; with `fit` false it
cancels the gun's order. The job is the same `RepairGear` job (`Job::bill` is the part's def id + 1 instead of 0) at a bench
with `fitsUpgrades: true` (the machining table), by a colonist with Smithing work and at least the part's `minSkill`, when the part lies within 12 m of the
bench. The part is used up at the end; the gun's `FittedParts` lists it. Parts cannot be taken off again (see below).

**Effect.** A gun with parts has a `WeaponMods` component (Combat.h), the product of its parts' factors, made again
from the parts on load. `combatGunOf` puts them on the def's gun for the shooter of that item: magazine size, reload time,
cooldown and warm-up (rounded to steps), recoil (the spread the shots add, and the gun kick list), and the accuracy
(multiplied into the spread's hit chance). The fighters' idea of the gun's reach (`FighterSystem::weaponRange`, which weapon
the AI picks by range) takes the range factor. Turrets use the def's numbers and ignore parts. The part is a per-item thing: a
gun stays upgraded when it is dropped, picked up or stowed.

## Snapshot, events, save

- `ThingView::repairMarked`, `fitOrder` (the part ordered, invalid for none), `firstFitted` and `fittedCount` into
  `Snapshot::fittedParts`; hit points were already in `hitPoints`.
- Events `GearRepaired` (`thing` pawn, `target` gear, `amount` hit points) and `WeaponUpgraded` (`target` the gun, `def` the part),
  and `PawnAction::Repair` when the work starts (reuse the crafting animation until there is one).
- Save section `repair` 1 ([saves.md](saves.md)): per item the mark, the order and the parts by defName. All three are in the state hash.
  Job type `RepairGear` was added at the end; a job in a save is checked like Refuel's.

## Not yet

- Taking a part off (or swapping one), and parts for melee weapons or apparel.
- A colonist fetching the material or the part (it is taken from near the bench), and a bill-like "repair everything under 50%"
  setting; the player marks each piece. Worn and carried pieces are not reached: a drafted fighter's gear must be put down.
- Armor and weapons wearing out in use: apparel already loses hit points when it absorbs damage (C15) but a weapon only
  deteriorates outdoors, so repairing weapons is rare until something wears them.
- Unreal: a repair mark icon, a parts list on the weapon panel, and a work animation (no art in the sim).
