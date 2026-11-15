# Armour, clothing and damage types

sneed's model (2026-11-14 and 2026-11-15), replacing RimWorld's armor rolls. Decision
[0009](../decisions/0009-mitigation-not-armor-rolls.md). The reviewed numbers are in the draft doc
"Armour and clothing rates" (sneed's thread "Rework armour, damage types and clothing quality").

Code: `Mitigation`, `DamageMix`, `mitigationDivisor`, `mitigationFactor`, `GearKind` and
`ApparelStats::mitigation` / `weightKg` in `ColonySim/Apparel.h`; `CombatSystem::mitigate` and
`armorPass` in `Private/Combat.cpp`. Tests: `Tests/ApparelTests.cpp` ("every worn piece divides the
damage", "subtypes and weaknesses multiply"), `MonsterTests`, `VanillaGearTests`.

## Damage types

Every hit is a mix of three classes: **blunt** (fists, clubs, falls, most of a bullet, blasts),
**sharp** (blades, arrows, bites, shrapnel) and **elemental** (fire, cold, toxic and acid, electric,
psionic; lasers are elemental with no subtype). The mix is the DamageTypes def's `mix`, and `explosionMix`
for the same type set off by an explosion:

| Damage type | Mix |
| --- | --- |
| Bullet | 75% blunt, 25% sharp |
| Cut, Stab, Scratch, Bite, Arrow | 100% sharp |
| Slash (sword edges: gladius, longsword, monosword, plasma sword) | 75% sharp, 25% blunt |
| Blunt, Crush | 100% blunt |
| Bomb (frag grenades, shells, rockets) | 75% blunt, 25% sharp |
| Burn | 100% elemental, fire; in an explosion (molotov, incendiary shell) 50% blunt, 50% fire |

The wound still comes from the damage type's `hediff` (a slash wounds like a cut).

## Mitigation

Every worn piece and a creature's hide have a % against each class (`blunt`, `sharp`, `elemental`) and
an extra % against each elemental subtype (`fire`, `cold`, `toxic`, `electric`, `psionic`). Damage is
divided by (1 + % / 100) for every counted piece, so two 10% pieces leave 100 / 1.1 / 1.1 = 83% and it
never reaches 0; there is no cap (500% leaves a sixth). A negative % multiplies the damage by
(1 + |%| / 100) instead (plant fibre's -8% fire; a mechanoid's -50% electric). An elemental hit with a
subtype uses both: elemental, then the subtype's extra. Each part of a mixed hit is divided by its own
class, then the parts add up.

**Where it lands doesn't matter.** Every counted piece works against every hit:

- Clothing: one chest piece (T-shirt, button shirt, jacket, parka or duster: the type is only a look),
  one pair of pants, one hat. Tribal wear covers chest and legs and counts twice.
- Armour goes over clothing: one head, one chest, one legs piece. Suits (plate, recon, marine,
  cataphract) cover chest and legs and count twice. The flak vest and flak jacket are the same piece.
- Head: only the better of the hat and the helmet counts, per damage class.
- Other gear (shield belt, pop packs) has no mitigation.
- A shield on the arm counts once, like a chest piece; slung behind a two-handed weapon, not at all
  ([shields.md](shields.md)).
- Last, the creature's own `naturalMitigation` (mechanoids, chitinids; animals have none yet).

Two pieces conflict only when they are the same kind (clothing or armour) and share a slot
(`gear.kind`, `gear.slots`); defs without `gear` keep RimWorld's layer rule.

**Armour penetration** (a bullet's `armorPenetrationBase`, a tool's `armorPenetration`) takes its share
off every positive %: a 50%-penetration hit leaves a 30% vest at 15%. Weaknesses stay.

Nothing deflects and no roll is made. The pieces that took damage off wear between them what a single
layer took before: 1 hit point's worth of durability per 4 points of the hit, split by each piece's share
of the reduction (`ApparelRules.absorbDamageDivisor`).

## Where the numbers live

- Clothing materials: `clothing.mitigation` and `clothing.kg` on the stuff (cloth, wools, leathers,
  devilstrand, synthread, hyperweave, the new `PlantFibre`). A garment takes them times `gear.share`
  (hats 0.5). Wolf and panthera leather weigh nothing.
- Armour: `gear.mitigation` on the def and `stats.mass` for its weight. Armour made of a material (plate
  armour, flak helmet) holds steel's numbers; the material's `armourStuff.factor` scales blunt, sharp and
  elemental and `armourStuff.offset` adds (wood: 60% sharp, no elemental, -10% fire).
- Creatures: `naturalMitigation` on the race def.
- Quality (`QualityRules.levels.armorFactor`) scales every positive %: awful 0.5, poor 0.75, normal 1,
  good 1.25, excellent 1.5, masterwork 1.75, legendary 2. Weaknesses are not scaled.
- Worn down (tattered, 0 durability): positive %s at 67% (`ApparelRules.tatteredMitigationFactor`),
  insulation at 80% as before; -1 mood for each tattered piece, up to -3
  ([apparel-durability.md](apparel-durability.md)).
- Insulation: every regular garment gives +10 cold and +2 heat degrees (`ApparelRules.clothingInsulationCold`,
  `clothingInsulationHeat`), times quality; armour keeps its own.

## Weight

What a pawn wears weighs on it (`ApparelStats::wornKg`, `armourKg`; numbers in `ApparelRules`):

- **Walking**: speed / (1 + kg / 100) for everything worn plus the weapon in its hands; a hat under a helmet
  weighs nothing. Clothing weighs its material's `clothing.kg` (a hat half), armour its `stats.mass`.
  5 kg is about 5% slower, a cataphract set (17 kg) about 15%.
- **Attacks**: aiming, the cooldown after a burst, reloading and melee swings take (1 + armour kg / 100)
  times as long. Clothing doesn't count.
- **Work**: every work speed (`JobSystem::workFactor`) / (1 + 0.25 x armour kg / 100): 20 kg of armour is
  1 / 1.05. Recreation is not work and isn't slowed.
- Worn gear and the weapon in hand are not in the carry load ([inventory.md](inventory.md)).
- The old `equippedStatOffsets.moveSpeed` on armour was never applied and is still not.

## Putting it on

Clothing goes on in one step; armour takes 120 steps a piece (4 seconds at 1x, `ApparelRules.armourWearSteps`),
and a new injury while putting it on stops the job ("hit while putting on armour"). A pawn picks clothing
first, then armour legs, chest, helmet, then other gear (`ApparelSystem::bestToWear`).

## Fight gear in the bag

An outfit can keep gear in the pawn's bag and put it on only for fights (`OutfitPolicy.armourInBag`,
`clothesInBag`; `SetOutfitFightGearCommand`; the Outfits def keys of the same names):

- Outside a fight the pawn picks up the best allowed pieces of that kind into its bag (as much as it can carry;
  a better piece swaps for the one in the bag, which drops) and takes worn ones off into the bag. Bag gear counts
  toward the carry load; worn gear doesn't.
- When it fights (drafted, called up by the alarm, or an enemy; not while fleeing) it puts them on from the bag:
  clothes all at once, then armour legs, chest, helmet at 120 steps a piece (the `Dressing` component). Meanwhile it
  can't shoot or swing ("putting on armour"). A new injury makes it give up and fight as it is until the fight ends.
- When the fight ends the job system puts the pieces back in the bag.
- `clothesInBag` leaves hats on (RimWorld's nudist outfit allows them). Outfits: "Armour for fights" (armour in the
  bag) and "Nudist, dressed for fights" (clothes and armour in the bag, hat on). The plain "Nudist" outfit stays
  naked but for a hat.

**Hide gear** (`HideGearCommand`, the `HideGear` component, `ThingView.hideGear`): a look-only switch; the pawn's
clothes and armour are not drawn but still count for everything.

## Not done yet (next steps in the same thread)

- Buttons for the fight gear and hide gear settings (the sim and the drawing are done; there is no outfit window
  in Unreal yet).
- Nudists dressed for work (sneed: "for fights or work").
- Races, strength or melee skill lowering the weight penalty (sneed: "various things affect this").
- Plant fibre has no source yet (BACKLOG). Gloves and boots don't exist yet.
