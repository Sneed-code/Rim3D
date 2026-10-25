# Aiming like a player: reaction, leading, reloading, dodging

Code: `ColonySim/Combat.h` (`reload`, `roundsLeft`, `reactionSteps`, `leadPoint`, the `Magazine`
component, the `Reloading` trigger phase) and `ColonySim/Fighters.h` (`reactUntil`, `dodgeUntil`).
Tests: `Tests/AimingTests.cpp`, the `Reload` command in `Tests/FighterTests.cpp` and `SaveTests.cpp`.
Data: `magazineSize` and `reloadTime` on each gun in `Things/Weapons.json`, four skill curves in
`CombatRules`, the reload and dodge numbers in `FighterRules`. Task F5. It stands on guns and
bullets ([combat.md](combat.md)) and the fighter AI ([fighters.md](fighters.md)).

The game design doc: "For AI fighters, Shooting skill sets how well they aim: tighter spread,
quicker reactions and leading moving targets." Spread and recoil by skill were F2's. F5 adds the
rest, and the parts of a shooter that aren't about the shot itself: magazines, and getting out of
the way of something slow.

## Skill curves

Each is given at Shooting skill 0 and 20 in `CombatRules` and is a straight line in between. All
ours: RimWorld has no reaction time, leading or magazines (its hit roll stands in for all of it).

| Rule | Skill 0 | Skill 20 | What it does |
| --- | --- | --- | --- |
| `reactionStepsBySkill` | 15 (0.5 s) | 5 (0.17 s) | steps from picking a new target to starting to aim at it, and from a projectile's launch to noticing it |
| `leadBySkill` | 0.4 | 1.0 | how much of the target's movement a shot allows for |
| `leadErrorBySkill` | ±0.4 | ±0.05 | how far off that guess may be, picked evenly per shot |
| `reloadTimeFactorBySkill` | 1.3 | 0.7 | times the gun's `reloadTime` |

Pawns without skills (raiders until F6) count as skill 4, as in F2.

## Reaction time

When a fighter's plan picks a target it didn't have before (the first one, a new one after the old
went down, an attack order), it may not start aiming until `reactUntil`, its reaction time later.
The gun's warm-up follows as before. Fighters spot enemies when they plan (twice a second), so the
moment a new enemy is noticed is up to half a second late on top: the plan's rhythm stands in for
the time it takes to notice something at all. A skill 20 rifleman shoots back noticeably sooner
than a novice (test "a fighter waits its reaction time"). The player's own trigger and the
`Shoot` command have no reaction time: the order is the reaction.

## Leading moving targets

An aimed shot at a pawn (a fighter's, or `ShootCommand` without `player`) aims where the target
will be when the bullet gets there: its chest plus its velocity times the flight time times the
lead (`CombatSystem::leadPoint`). The flight time is distance over bullet speed less half a step,
because a bullet flies its first stretch in the step it is fired and the target moves at the start
of each step after; twice round, since leading changes the distance. The drop is aimed for as
before. The lead is `leadBySkill` plus a random error of up to `leadErrorBySkill` (drawn only when
the target moves, so standing fights are unchanged).

At 22 m a walking man moves about half a metre while an autopistol bullet is on its way, more than
he is thick seen from the side, so without leading nearly every shot passes behind him. A skill 20
shooter hits a man walking past about as often as one standing side on (test: 29% against 25%,
both side on); a skill 0 shooter hits him 2% of the time. Seen from the side a man is a smaller
target than from the front, which the hitboxes take care of by themselves.

## Magazines and reloading

Each gun has `magazineSize` rounds and a `reloadTime` in steps (ours, roughly real guns at
RimWorld's pace):

| Gun | Rounds | Reload |
| --- | --- | --- |
| revolver | 6 | 120 steps (4 s) |
| autopistol | 15 | 60 (2 s) |
| bolt-action rifle | 5 | 105 (3.5 s) |
| pump shotgun | 6 | 150 (5 s) |
| assault rifle | 30 | 75 (2.5 s) |

A gun without `magazineSize` never reloads. The rounds left are a `Magazine` component on the gun
item, added the first time it fires: a gun keeps its count when it is dropped and picked up.

- **Running dry.** The shot that empties the magazine ends the burst there, and the trigger goes to
  `Reloading`: the gun's cooldown, then `reloadTime` times the skill factor. A revolver at skill 0
  is ready again 48 + 156 steps after its sixth shot, at skill 20 after 48 + 84. A `PawnAction`
  `Reload` event goes out when it starts (`target` the gun); `shotReadyIn` counts down the whole
  wait, and the snapshot has `rounds`, `magazineSize` and `reloading`. `Shoot` is refused with
  "reloading" meanwhile. A downed pawn's reload stops and goes on when it gets up.
- **Reloading early.** `CombatSystem::reload` (the `Reload` command, the protagonist's reload key
  once U11 binds it) starts one when the trigger is ready or cooling down and the magazine isn't
  full; during a cooldown the reload is added to what is left of it.
- **Fighters reload in cover.** Between shots, at its spot, with no shot to take, a fighter tops
  up: when it knows of enemies, below half a magazine (`reloadBelow`), when it knows of none,
  whenever it isn't full. Reloading counts as not ready, so a fighter that stepped out to shoot steps
  back into cover and crouches while it reloads. Walking doesn't stop a reload.

## Sidestepping slow projectiles

Every step, before planning, a fighter (not one the player controls, not mid-burst) looks at the
projectiles in flight. One coming at it is a danger when:

- it was fired at least the fighter's reaction time ago (it has noticed it);
- its closest approach on the ground is ahead and within `dodgeRadius` (0.6 m) of the pawn's
  centre, and at that moment it is between the ground and just over the pawn's eyes;
- the pawn can still move `dodgeClearance` (0.9 m) away from its line before it arrives, at its
  walking speed (injuries count).

For the nearest such danger the fighter takes a one-cell step away from the line (or across it
if that side is blocked and the step is long enough), drops an aim still in its warm-up, sends a
`PawnAction` `Dodge` event (`target` the projectile, `cell` where it steps) and does nothing else
until the projectile has passed plus `dodgeHoldSteps` (3). Then its plan runs again and it goes
back to its spot.

Bullets are never dodged: a rifle bullet at 210 m/s is 35 m further on after even a skill 20
pawn's reaction time, so it would have to be fired from beyond any gun's range. Thrown and slow
things (grenades, arrows, rockets, none written yet) will be. The test uses a made-up 18 m/s
projectile fired straight at a drafted colonist from 18 m: it steps aside, the shot misses, and
it walks back; the same with a rifle bullet hits it.

## Saves, hash, replays

Save format 17: the `Magazine` component, the `Reloading` phase, `Fighter::reactUntil` and
`dodgeUntil`, and command 27 (`Reload`) ([saves.md](saves.md)). Older saves load with full guns. All
of it is in `Sim::stateHash`; `Reload` is in replay files.

## Cost

The dodge check is fighters times projectiles in flight each step, a few multiplications each; in
the 30 against 40 test the whole sim stays at about 0.4 ms a step in the cloud (0.35 before F5,
within the noise of the machine).

## Not done yet

- Players' sway, and the talent tree's "steadier aim" (the protagonist's skill already reduces
  recoil and speeds reloads through the same curves).
- Ammunition as items: magazines refill from nowhere.
- Diving away from grenades and blast radius (grenades aren't in yet); dodging by going prone.
- Reacting to a target that pops up from cover: the fighter AI shoots at where an enemy will
  show itself (fighters.md), so it has no separate "it just appeared" moment yet.
- Unreal: a reload animation and sound for `Reload`, a sidestep for `Dodge`, the reload key (U11).
