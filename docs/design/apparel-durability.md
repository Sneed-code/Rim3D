# Apparel durability: clothes wear out, they don't fall apart

sneed's rule (2026-10-27), on top of [apparel.md](apparel.md) (C15) and [repair.md](repair.md) (P4). It deliberately
changes RimWorld, where worn apparel loses hit points and is destroyed at none.

Code: `Durability` and `ApparelStats::condition` / `tattered` in `ColonySim/Apparel.h`, the wear in
`Private/Apparel.cpp` (`ApparelSystem::step`, `wearDown`) and `CombatSystem::damageApparel` (`Private/Combat.cpp`), the
repair cost in `RepairSystem::costOf` (`Private/Repair.cpp`). Data: `ApparelRules` in `Rules/Rules.json`, the apparel
numbers in `Rules/RepairRules.json`, `apparel.fragile` on basic clothes in `Things/Clothing.json`, the `TatteredApparel`
thought in `Thoughts/Thoughts2.json`. Tests: `Tests/ApparelTests.cpp`, `Tests/RepairTests.cpp`.

## Two numbers on a piece

- **Hit points** are the item itself. They go when it is *not* worn: left outside it deteriorates, fire and explosions
  hit it, a smelter or crematorium destroys it. At 0 it is gone. Worn apparel does not lose hit points (one exception
  below).
- **Durability** (`Durability::share`, 0 to 1) is wear. Wearing it, the hits its armor takes and its wearer dying wear
  it down. It can reach 0 with the hit points untouched.

Durability is capped by the hit-point share: an item at 70% of its hit points is at most 70% durable
(`ApparelStats::condition` is the smaller of the two). So damage to a lying item lowers both. An item without the
component is fully durable.

## What wears it

| What | How much | Number |
| --- | --- | --- |
| Being worn | RimWorld's 0.4 hit points a day, as a steady rate of durability, checked hourly: a 100 hit point shirt wears 24% in a 60-day year, an 80 hit point hat 30% | `wearHpLossChancePerDay` 0.4 (RimWorld) times `wearFactor` 1 (ours, to tune) |
| Armor absorbing a hit | 1 hit point's worth per 4 damage absorbed (RimWorld's rate) | `absorbDamageDivisor` 4 |
| The wearer dies | 15 to 40% of the durability it has | `deathHpLoss` |

All in `ApparelRules`. Durability is a share, so a sturdier piece (more hit points) wears more slowly.

## Tattered

At 0 durability a piece is **tattered**: it stays on, but its armor and insulation are 80% of normal
(`tatteredBenefitFactor` 0.8), and its wearer has the thought "tattered clothes", **-1** (one stage; RimWorld's ratty
-3 and tattered -5 are gone). It looks the same for now: the snapshot gives `durability` beside `hitPointShare`, and
the clothes are drawn torn by hit points as before.

Outfits' hit point range (`hitPointsMin`, `hitPointsMax`) and the score a pawn gives a piece now use durability (the
fields keep their names). A pawn whose outfit starts at, say, 25% swaps out a worn-down piece for a better one.

## Basic clothes can be destroyed

Apparel with `apparel.fragile: true` (T-shirt, button-down shirt, pants, tuque) behaves the same until its durability is
0; after that the rest of each hit's wear comes off its hit points, and at 0 hit points it is destroyed (event
`ApparelDestroyed`). Normal wear never does this; only hits do.

## Repair

The P4 repair job ([repair.md](repair.md)) mends apparel back to full hit points **and** full durability in one go.
What it costs is set by the bigger loss: `1 - condition`, so lost hit points and lost durability cost the same. For
apparel (all ours, `RepairRules`):

- material: `apparelMaterialShare` 0.1 of `costStuffCount` of the stuff it is made of, times the share lost, **rounded
  down**, at least 1. A 35-cloth shirt worn out costs 3 cloth; a 40-cloth T-shirt 4.
- work: `apparelWorkShare` 0.1 of `workToMake` times the share lost (a T-shirt: about 4 game minutes from worn out).
- apparel with no stuff (flak vest, helmets): 1 unit of cloth (`apparelMaterialPerKg`) and 100 work
  (`apparelWorkPerKg`) per kg for a full repair.

Weapons keep the P4 numbers (a quarter). A piece must still be put down to be marked (P4); a worn-out piece is marked
like any damaged one.

## Save and hash

Save section `apparel` 2: per item `hasDurability` and `durability` ([saves.md](saves.md)). Version 1 saves load with
every piece fully durable. Durability is in the state hash.

## Not yet

- Colonists taking off a tattered piece by themselves to have it mended, or a "repair everything worn" order.
- A tattered look (sneed: not for now).
