# Turrets and traps

Code: `ColonySim/Defense.h`, `Private/Defense.cpp`; the shot itself is `CombatSystem::fireFrom`
(`Private/Combat.cpp`), the avoiding is in `Private/Fighters.cpp`. Tests: `Tests/DefenseTests.cpp`.
Data: `Things/Defense.json` (the mini-turret and its gun, the spike trap, the IED),
`Projectiles/TurretBullets.json`, `Rules/DefenseRules.json`, the `GunTurrets` project in `Research`.
Task F11. It stands on combat ([combat.md](combat.md)), explosives ([explosives.md](explosives.md))
and power ([power.md](power.md)).

The rule of the game: **the sim decides who is hit and who is stabbed, Unreal shows it.**

## The mini-turret

A building whose def has a `turret` block (`gun`, `shootingSkill`, `muzzleHeight`) gets a `Turret`
component. It is a powered consumer (100 W, `power.consumption`) like any other: with no power, or
switched off (`FlickCommand`), it forgets its target and its burst and does nothing.

- **Looking.** Every `scanEvery` steps (10, staggered by building id) an idle turret picks the nearest
  pawn that is able (not downed, not dead), not an animal, not on the colony's side (`kPlayerFaction`),
  within the gun's range, and seen: a line from the muzzle to the pawn's chest that ground, rock and
  buildings do not stop (`CombatSystem::trace`; the turret's own cell never blocks).
- **Shooting.** Warm-up (the gun's `warmupTime`), a burst of `burstShotCount` shots `secondsBetweenBurstShots`
  apart, then the gun's `rangedWeaponCooldown`. Every shot is a real projectile from `fireFrom`: the
  muzzle is `muzzleHeight` above the ground, the aim leads a walking target (`lead` 1: perfectly), and the
  spread comes from `CombatSystem::spread` with the turret's `shootingSkill` (8, ours) and the gun's
  accuracy table, so range, cover and armor work as for any gun. The target is checked every step; if it
  is downed, dead, out of range or hidden the turret cools down and looks again. The projectile's
  `launcher` is the turret, so ShotFired events say `thing` = the turret.
- **Mini-turret numbers** (all `unverified`: recalled, not checked on the wiki): 100 HP, 100 W, 100 steel and
  2 components, gun range 28.9 m, warm-up 30 steps, burst of 3, 5 steps apart, cooldown 144 steps, bullet
  12 damage, 0.15 penetration. Research `GunTurrets` (needs Machining).
- **Blowing up.** A building whose def has an `explosion` block and `explodeWhenDestroyed` goes up when
  *combat* destroys it (shot or blasted to pieces): `DefenseSystem::onDestroy` is part of
  `CombatSystem::setBeforeDestroy`'s hook, queues the blast, and the same step's `DefenseSystem::step` calls
  `CombatSystem::explode` for it (a chain of turrets goes on over a few steps, at most 64 blasts a step).
  Deconstruction and `DestroyThingCommand` never explode. The turret's blast (25 damage, radius 2.9 m)
  is ours; RimWorld's is not recalled.

## Heavy turrets (autocannon, uranium slug)

`Turret_Autocannon` (`AutocannonTurret`) and `Turret_UraniumSlug` (`UraniumSlugTurret`) are mini-turrets with RimWorld's
wiki numbers (checked 2026-10-28; `Things/DefenseTurrets.json`, `Projectiles/TurretBullets2.json`), 2 x 2, 150 W,
380 HP, 70% flammable, with two additions to the turret code:

- **Ammunition** rides on the fuel system ([fuel.md](fuel.md)): the def's `fuel` block (steel, capacity 180, or uranium,
  capacity 60) with `perUse` 2 (ours, from the wiki's 90 and 30 rounds) is what a shot takes. A turret with no
  ammunition holds its fire; one that runs out mid-burst cools down. Colonists refuel it like any building (the
  Refuel giver, fetching the item from a stockpile). `DefenseSystem::setFuel`.
- **Minimum range** (`verbs[0].minRange`, 8.9 and 11.9 m): a target closer than that is not picked, and a target that comes
  closer is dropped. `GunStats::minRange`.

Autocannon: range 32.9 m, burst of 3 with 8.5 steps between, cooldown 105 steps, 27 damage at 40% penetration, accuracy
28/72/66/50%, warm-up 45 steps (ours: the wiki gives none). Slug: range 45.9 m, one shot per 120 steps, no warm-up,
55 damage at 82% penetration, accuracy 22/33/44/95% (the last is the wiki's unlabelled 95%, taken as long range).
Their blast (50 Bomb, 5.9 m) goes off when they are destroyed; RimWorld's comes 4 s after they drop below 20% HP.
Tests: `HeavyTurretTests` (shots, ammunition, minimum range, a 2 x 2 turret shooting out of all 8 directions).

## Traps

A building whose def has a `trap` block gets a `Trap` component (`sprung`, `known`). It is not a wall
(`passability` is the building default), so anyone can walk over it.

- **Springing.** Every step, a living pawn whose cell is an armed trap's cell, and that does not know the
  trap, springs it (one pawn per trap per step). **Knowing:** every pawn of the colony's side always
  knows every trap, so colonists and tame animals never spring them. A pawn of another side knows a trap
  once its side has *learned* it (below). A pawn that knows a trap springs it only with
  `knownSpringChance` (0, ours). Wild animals are of their own side and spring traps.
- **Spike trap** (`trap.hits` 5, `damage` 100, `damageDef` Stab): 5 stabs of the total
  (70% to 130% of 100, random) divided evenly, each on a random outside part picked by size
  (`CombatSystem::strikePawn`) and through the pawn's armor like a bullet (`trap.armorPenetration` 0.5, ours),
  so a vest and helmet turn stabs away and wear. After it springs it is `sprung` until rearmed
  (`RearmTrapCommand`, snapshot `trapState` 2) and has `destroyChance` (0.1, ours) to break instead.
  Cost 25 steel (unverified).
- **IED** (`trap.explodes`, `explosion` Bomb 45 damage, radius 3.9 m, unverified): springing calls
  `CombatSystem::explode` and the trap is gone. It can't be rearmed. One shot or blasted to pieces goes off
  once (a sprung trap is skipped by `onDestroy`).
- **Learning.** The side of the pawn that sprang a trap learns it (kept in `Trap::known`, sorted faction
  ids). Fighters of other sides also *notice* armed traps: every `noticeEvery` steps (15) each fighter within
  `noticeRadius` (4 m) rolls `noticeChance` (0.35); on success its whole side learns it and all of
  that side's fighters plan again (`onLearn` -> `FighterSystem::replan`). All four numbers are ours.
- **Avoiding.** `DefenseSystem::avoidsCell(faction, cell)` is true for an armed trap the side knows.
  The fighter AI does not pick such a cell as a spot (`plan`, `planGoal`) and bends every walk round
  them (`DefenseSystem::detour`, called after `smoothPath`): where a straight stretch of the route
  crosses a known trap's cell (and `avoidMargin`, 0.15 m, round it), it inserts the walkable cell within two
  cells of the trap that makes the shortest detour and misses every known trap, up to `detourTries` times.
  A walk with no way round goes through. The colony's own fighters never avoid anything.

## Rearming

The `RearmTraps` giver (Construction work type, `DefenseJobs.cpp`) offers every sprung trap that can be used again,
nearest first, one colonist per trap. The `RearmTrap` job walks next to the trap and works `trap.rearmSeconds` (13,
ours: RimWorld's 800 ticks) of game seconds, each second adding the pawn's Manipulation, then calls `DefenseSystem::rearm`
(event `TrapRearmed`, `PawnAction::Rearm` when the work starts). An IED is never offered. `RearmTrapCommand` still
works for the player.

## The snapshot, events and commands

- Snapshot `ThingView`: `turretPhase` (`TurretPhase`: Idle, Warming, Firing, Cooling), `turretTarget`,
  `trapState` (0 not a trap, 1 armed, 2 sprung).
- Events: `ShotFired` (a turret's shots), `TrapSprung` (`thing` the trap, `target` the pawn, may be gone for an
  IED), `TrapRearmed`, then `Explosion`, `PawnInjured` and the rest as they happen.
- Command: `RearmTrapCommand{trap}` ("RearmTrap" in replay files). Fails for a thing that is not a trap, a trap
  that is armed already, or an IED.
- Save section `defense` 1: per building `turret` (phase, steps, shots left, target) and `trap` (sprung, the
  sides that know it). Both are in `Sim::stateHash`. Saves from before the section load with fresh turrets
  and armed traps.

## Not done yet

- Raiders do not shoot back at turrets: the fighter AI chooses only pawns as targets, so a turret is
  hit only by stray fire, blasts and what T1/F12 add (sappers, mortars).
- IEDs wait for chemfuel versions (E4); no EMP, no incendiary traps; no sentry
  machine-gun turrets. Mortars and the rocketswarm launcher (manned, ammunition, indirect fire) are not in.
- Unreal: the turret mesh and its barrel turning to `turretTarget`, muzzle flash and the trap's spring
  animation.
