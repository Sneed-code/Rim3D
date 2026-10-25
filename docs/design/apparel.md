# Apparel: clothes and armor (C15)

Code: `ColonySim/Apparel.h`, `Private/Apparel.cpp` (items, outfits, wearing, wear and tear, taint,
thoughts), `Private/ApparelJobs.cpp` (the jobs), armor in `Private/Combat.cpp`. Tests:
`Tests/ApparelTests.cpp`. Data: `data/defs/Outfits/Outfits.json`, `Rules/Rules.json` (`ApparelRules`),
the clothes thoughts in `Thoughts/Thoughts2.json`.

## A piece of apparel

An apparel item is a Thing with `HitPoints` and, for a garment made of a stuff (cloth, leather,
devilstrand...), `MadeOf{stuff}` and `Quality{level}` (0 awful to 6 legendary; no `Quality` means
normal). A worn piece is off the map and listed in the wearer's `Gear::apparel`.

**Armor** (`ApparelStats::armor`): a garment made of a stuff has the stuff's armor factor times the
garment's `armorMultiplier` (from `Textiles.json` and `Clothing.json`, D6); other apparel (flak
vest) has its own rating; both times the quality's factor (`QualityRules`). **Insulation** follows
the same way from `insulationCold` and `insulationHeat`. Hit points are the def's times the stuff's
factor. Without a stuff a `stuffRequired` garment is worth nothing.

**Combat** uses the same rule for every piece: per covered part, outermost layer first, a roll under
half the armor deflects, under all of it halves the damage (F2, `combat.md`). Every layer that
absorbs damage loses 1 hit point per 4 damage absorbed (random rounding). Bullets and melee blows (F8) share `CombatSystem::hitPawn`, which lists the pieces that absorbed (`PawnHitResult::wear`); the caller applies it with `wearApparel`. At 0 hit points the piece
is destroyed (`ApparelDestroyed` event).

## Wear and tear, tattered, dead men's clothes

- Each worn piece loses 1 hit point with a 0.4 chance per day (checked hourly).
- Under 50% of its hit points a worn piece is ratty (thought -3), under 25% tattered (-5): the worst
  piece counts (`TatteredApparel`, RimWorld wiki).
- When a human dies wearing something it loses 15 to 40% of its hit points and is `Tainted` for good.
  Wearing tainted clothes gives "wearing a dead man's clothes": -5, -8, -11, -14 for 1, 2, 3, 4 or
  more tainted pieces, with no duration; Bloodlust pawns don't mind. Taking the clothes off ends it.
- Naked (RimWorld's "psychologically nude": a man with his legs uncovered, a woman with her legs or
  chest uncovered): -6. Nudists get +20 naked and -3 when
  clothed (`NudistNaked`, `NudistWearingClothes`).

The thoughts are Thoughts defs whose `situation.condition` (`noApparel`, `apparelOnNudist`,
`apparelTattered`, `apparelTainted`) the apparel system answers: `MoodSystem::addSituation` registers
a function that gives a stage number; `situation.trait` and `situation.notTrait` limit a thought to
pawns with or without a trait. 
## Outfits

An outfit policy (`Outfits.json`: Anything, Worker, Soldier, Nudist) lists the apparel defs it
disallows, a hit-point share range and a quality range. Ids are 1, 2, 3... in the order made. A pawn
follows its `PawnOutfit` (default: the first outfit, "Anything"). Commands: `SetOutfit{pawn,
outfit}`, `EditOutfit{...}`, `AddOutfit{name}`, `RemoveOutfit{outfit}` (pawns on it go back to the
default; the first can't go). The snapshot lists the outfits and each pawn's outfit.

## Changing clothes (think node)

`thinkApparel` sits after the schedule and before work. A pawn with a worn piece its outfit doesn't
allow takes it off first (job `WearApparel` with `count` 1); else it looks for the lying apparel
that gains most (`ApparelSystem::score`: armor by type, insulation, hit points, taint penalty) by at
least `minGain` within 50 m (ours) that it can reach and nobody else reserved, walks there and puts
it on (60 steps per piece; what it replaces drops where it stands). It keeps looking while it
finds things; once nothing is found it waits 1,200 steps (`nextCheck`). Scenario colonists start
dressed (`colonistApparel`), nudists not. Controlled colonists are left alone like every think node.

## Stripping

`StripCommand{thing, strip}` marks a downed or dead pawn. A colonist with Hauling work walks to it
and takes the apparel and weapon off, 60 steps a piece, leaving them on the ground (`StripGiver`,
job `StripPawn`; the work type's tasks are now Refuel, Strip, HaulToStockpile). The mark goes when
nothing is left, or the pawn gets up.

## For later tasks

- E2 (temperature): `ApparelSystem::insulation(pawn, cold, heat)` gives the degrees the comfortable
  range widens.
- C14 (crafting) makes apparel with `ApparelSystem::make(def, stuff, quality, cell)`.
- P4 (repair) mends hit points; U16 draws `Snapshot::apparel` layers, torn by `hitPointShare`.

## Save and hash

Section `apparel` 1: per thing `SaveApparel` (stuff, quality, tainted, strip mark, outfit,
`nextCheck`), the outfits and the next outfit id. The five commands are saved by name. Worn items are
saved as before through `Gear`.
