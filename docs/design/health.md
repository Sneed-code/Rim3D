# Health

Code: `ColonySim/Health.h`, `Private/Health.cpp`. Tests: `Tests/HealthTests.cpp`. Data: `Bodies`,
`BodyParts`, `Capacities`, `Hediffs`, `DamageTypes` and `Rules/HealthRules` in `data/defs/`
(numbers and where they come from: [starting-data.md](starting-data.md)). Task F1.

Every pawn whose def names a body (`race.body`) gets a `Health` component. Hurt it, and it
bleeds, hurts, slows down, goes down, may die, and heals slowly. Nothing here is drawn or decided by
Unreal: it reads `downed` and `dead` from the snapshot, health events from the event list, and a full
report from `Sim::healthReport`.

## The body

A `Bodies` def is a tree of parts (torso at the root). The sim reads it into a flat list in
depth-first order, so **a part is an index** (torso 0; the Human has 64) and a part's children are the
indexes right after it. Each part has `hitPoints`, `bleedRate`, `solid` (bones), `destroyableByDamage`
and `lethalIfDestroyed` from its `BodyParts` def, a `coverage` (its share of its parent) and a depth:
**Outside** (skin: arms, head) or **Inside** (organs and bones under another part).

## Injuries

`Health` keeps a list of **injuries** (a part, a Hediffs def like Cut or Gunshot, and `severity`, the
hit points lost) and a list of **lost parts** (a destroyed part; its children go with it). A part's
hit points are its maximum (doubled for the protagonist, `Health::protagonist`, set before the pawn
is hurt) minus the severity of the injuries on it; efficiency is that over the maximum, 0 if lost.

**`HealthSystem::applyDamage(pawn, damageType, amount, part, rng)`** is the one way in. Combat (F2) will
call it; until then the dev command `DamagePawnCommand` does.

- **Which part.** An explicit `part` is hit exactly. With `part = -1` (what a bullet or a bite does),
  the part is picked by how much of the body each *outside* part covers (its area minus its outside
  children's; a lost part's share goes to the rest). If the damage type `reachesInside` (Bullet,
  Cut, Stab; **ours**, set in `DamageTypes`), the hit then goes in to an organ or bone under it with the
  chance of those parts' coverages added up, and again below that (head, skull, brain). Blunt, bites,
  burns and the rest stay on the outside.
- **Inside hits hurt the outside too.** When a random hit lands on an inside part, the outside part
  over it takes the same damage (`HealthRules.innerInjuryAlsoHurtsOuter`, unverified).
- **What happens to the part.** Damage at least as big as the part's remaining hit points destroys it
  (it becomes a lost part, its injuries and its children's disappear) if `destroyableByDamage`; a
  skull, ribcage or pelvis is left with 1 hit point instead. Smaller damage is an injury, of
  the damage type's `hediff` (its `hediffSolid` on bones, so a blow to a bone is a crack). Injuries of
  types that `canMerge` (shredded, crush, burn) add to an untended one already on the part.
- Events: `PawnInjured` (def, part, amount) or `PawnPartLost`; then `PawnDowned` or `PawnDied` if that
  followed. All come out in the step the damage is dealt.

## Pain, bleeding, blood loss

- **Pain** is the sum over injuries of severity x the hediff's `painPerSeverity`, plus lost parts that
  are still untended (hit points x the missing part's `painPerSeverity`, **ours**), divided by the body's
  `baseHealthScale`.
- **Bleeding** is the sum over *untended* injuries of severity x the hediff's `bleedRate` x the part's
  `bleedRate`, plus untended lost parts (twice their hit points x 0.12 x the part's `bleedRate`).
  Bones and bruises don't bleed. It is a rate of blood loss per day.
- **Blood loss** (0 to 1) rises each second by that rate / 86,400 while it is at least 0.1 a day, and
  otherwise recovers at 0.333 a day. Its stages (from the BloodLoss hediff) subtract from and cap
  Consciousness; at 1.0 the pawn dies. A fresh 30 point gunshot to the torso, left alone, takes
  about 13 game hours to kill and has the pawn down for the last 5.
- **Tending** (`tendAll`, or `TendPawnCommand`) stops an injury or lost part bleeding and hurting
  (lost parts) and adds `tendedHealBonusPerDay` to its healing, by quality. Doctors and medicine come
  later (not in Stage 1); until then only the dev command tends.

## Capacities

Each `Capacities` def is worked out from the parts and from the other capacities (the rules are in
starting-data.md), then blood loss's offsets and caps are applied. The pain factor on Consciousness
is `min(1, 1.05 - 0.45 x pain)`. Moving and Manipulation use **limbs** (`"limbs": {core, segments,
digit, digitWeight}`): each leg or arm is its core part's efficiency x each segment's x
`(1 - digitWeight) + digitWeight x (share of working toes or fingers)`, and the capacity is the mean over
the limbs, so one leg gives 50% moving and one arm 50% manipulation, as in RimWorld. (The first draft of
the data multiplied every part together, which made one lost leg 6% moving.)

## Down and dead

- **Downed** when Consciousness is under 30%, Moving is 15% or less, or pain is over 0.8
  (`painShockThreshold`). A downed pawn stops where it is (its route is cleared, the movement system
  keeps it still but others steer round it) and gets up again when none of those hold. Events
  `PawnDowned` and `PawnGotUp`.
- **Dead** when a part with `lethalIfDestroyed` is gone (torso, neck, head, brain, heart, liver), a
  capacity with `lethalAtZero` reaches 0 (Consciousness, Breathing, BloodFiltration, BloodPumping,
  Digestion), or blood loss reaches 1.0. `PawnDied` carries the cause in `message`
  ("blood loss", "a vital part destroyed", "organ failure"). Dead is permanent and saved. A dead pawn
  stays where it fell as a pawn with `dead` set, which is the corpse for now; Unreal lies it down.
  The game-over rule for the protagonist belongs to whoever builds the game flow.
- **Walking speed** is the Moving capacity: `Health::movingFactor` multiplies the pawn's speed in the
  movement system. The hauling placeholder drops its job when its pawn goes down (put the item down,
  free the other); C1's job system must do the same for every job.

## Healing

Every 864 game seconds (0.24 game hours, `healIntervalHours`; each pawn at its own offset so they don't
all heal in one step) one random injury of the pawn heals by `(naturalHealPerDay + rest bonus + tend
bonus) x 0.01` hit points: 8 a day, +4 on the ground, +8 in a bed (`Health::rest`; the bed system sets
it), +4 to +12 when tended. Which injury is chosen comes from a hash of the game seed, the pawn's id and
the game second, not the shared random stream, so the order pawns are visited in can't matter. An
injury at 0 is gone. Lost parts never come back (prosthetics and bionics are later). **Not built
yet:** scars, infections and diseases, immunity, permanent injuries, medicine items, the doctor job.

## Using it

```cpp
sim.health().applyDamage(pawn, sim.health().damageTypeId("Bullet"), 12.0f, /*part*/ -1, rng);
sim.health().tendAll(pawn, 1.0f);
HealthReport r; sim.healthReport(pawn, r);   // injuries, lost parts, capacities, pain, bleeding
sim.things().registry().get<Health>(pawn).protagonist = true;   // at set-up, before any damage
```

Pass the sim's shared random stream (`rng_`) from inside a sim system, so the roll is part of the
game; the dev command does. `Sim::stateHash` and the save include `Health` (save format 4). The derived
numbers (pain, bleeding, capacities, downed) are worked out again after every change and after
loading, so they are not saved.
