# Explosives and grenades

Code: `ColonySim/Explosives.h`, `Private/Explosives.cpp` (belts, throwing, flying and fuses),
`CombatSystem::explode` in `Private/Combat.cpp` (the blast itself), the dive and the throw decision
in `Private/Fighters.cpp`. Tests: `Tests/ExplosiveTests.cpp`. Data: `Things/Explosives.json` (the
frag grenade item), `Projectiles/Grenades.json` (what it throws and how it explodes), `DamageTypes`
(`Bomb`), `Rules/ExplosiveRules.json`, the F9 numbers in `Rules/FighterRules.json`. Task F9. It
stands on combat ([combat.md](combat.md)), health ([health.md](health.md)) and the fighter AI
([fighters.md](fighters.md), [aiming.md](aiming.md)).

The rule of the game: **the sim decides who is hurt, Unreal shows it.** A blast is worked out in the
sim, from walls, cover and body parts; Unreal's debris and ragdolls are for looks only.

## The blast (`CombatSystem::explode`)

`Explosion{x, y, z, level, radius, damage, damageType, penetration, source, instigator, seed}`.
Anything can call it: a grenade, `ExplodeCommand` (dev), later an IED trap (F11), a boomalope's
death (N5, the `deathExplosion` block in the animal def) or a bomb.

- **Pawns** (living, in id order). Three sample points on each (chest, feet, head of the stance it is
  in) are tested against the blast point, 0.3 m above the ground, with a line that ground, rock and
  buildings stop (pawns do not). Each point the blast sees and that is within the radius counts,
  worth `1 - (1 - edgeFactor) x distance / radius`: full damage at the centre, 40% at the edge
  (`edgeFactor`, ours: RimWorld's explosions have no falloff). The pawn's damage is 50 x the mean
  over the three points x a stance factor (standing 1, crouching 0.85, lying 0.5, ours: a man
  on the ground presents less to the fragments).
- **Fragments.** Bomb damage of 15 or more comes as 2 to 4 pieces of random sizes, each on a random
  outside part by its share of the body, each through the pawn's armor exactly like a bullet
  (`armorPass`, [combat.md](combat.md)) at the grenade's 10% penetration, then into health as `Bomb`
  damage (the `Shredded` injury). A piece that armor turns away does nothing.
- **Knock-down.** A pawn that takes 8 or more (before armor) is thrown to the ground: prone for
  20 steps plus 0.6 per damage (at most 90), pushed up to 1.5 m away from the blast if the way is
  clear, its path cleared, its aim dropped. While down it can't walk (the movement system holds it),
  shoot, throw, change stance or be ordered by the fighter AI; it gets back its old stance after.
  `KnockedDown` is the component; the snapshot's `knockedDown` and the `PawnKnockedDown` event say
  so. Jobs of a downed colonist go on ticking; a work job in place keeps its progress.
- **Buildings** with hit points, each once, by the nearest of its cells inside the radius that no
  other building shields (a grid walk between the cells): damage x falloff x factors from the damage
  type: x2 for buildings, x2 more if it blocks movement, x2 more for a wall (RimWorld's 2, 4 and 8):
  a frag grenade does 50 x 8 = 400 to a wall next to it, which has 300. Destroyed buildings go
  through the usual hooks (jobs let go) and `ThingDestroyed`.
- Not yet: plants and items in the radius, fire (molotovs wait for E4), chain reactions between
  grenades, mining bombs (a rock cell is never hurt), a blast above or below ground level.

Events: `Explosion` (centre, radius, damage type, who threw it) first, then `PawnInjured`,
`PawnPartLost`, `PawnDowned`, `PawnDied`, `PawnKnockedDown`, `ThingDestroyed` as they happen.

## Grenades

A grenade is a Things item with a `thrown` block: `projectile`, `range` 12.9 m, `warmupTime` 45
steps (90 ticks), `cooldown` 80 (160 ticks), `missRadius` 1.9. The **frag grenade** (needs
Machining; wiki: 265 silver, 1 kg) throws `Proj_GrenadeFrag`: `thrown.speed` 0.4 m a step (12 m/s,
not multiplied by tuning's x3 for bullets), `thrown.fuse` 50 steps after it lands (100 ticks), and
`explosion`: Bomb, damage 50, penetration 0.1, radius 1.9. Items stack to 10 (ours). The recipe
waits for chemfuel (C14/C11 crafting).

**Belt.** `Belt` on a pawn lists stacks of grenades, at most 10 in all (`beltLimit`, ours). Put a
stack on it with `EquipCommand` (a grenade item on the map: as many as fit are taken, the rest stay),
or `ExplosiveSystem::give` for kits (raiders in F6, tests). `DestroyThing` drops the belt where the
pawn stood. A dead pawn keeps its belt for now, as it keeps its gun.

**Throwing.** `ThrowCommand{pawn, item, target, x, y, player}`:

1. Wind-up (`Thrower`, apart from the gun's trigger): 45 steps, 0 for the player's own throw
   (`playerWarmup`). The pawn faces the aim; a `PawnAction` `Throw` starts it. It is refused while
   shooting, downed, knocked down or cooling down.
2. Release: the aim is the ground under the target pawn (a walking pawn is led by the
   Shooting-skill curve 0.4 to 1.0, twice round) or the point. The throw is never exact: the aim is
   moved a random distance in a circle of `missRadius` x min(1, distance / 9 m) x skill factor (1.0
   at skill 0, 0.5 at 20; x0.6 more for the player), all ours but the 1.9. Beyond the arm's range the
   throw goes as far as the arm does. `planThrow` picks the arc: the speed is fixed, so it takes
   the flat arc of the two that reach, the steep one if the flat is blocked, and says whether the
   grenade really comes to rest within a metre of the aim (`reaches`). `GrenadeThrown` event.
3. Flight: the grenade is an entity with a `Grenade` component (no `Thing`) in the snapshot's
   `projectiles` (fuse -1). Each step it moves with gravity and stops at the first ground, rock,
   building or pawn along its path (the thrower and its own cell excepted). It rests on the ground
   there; against a wall or rock a quarter metre short of it; on top of low cover (sandbags) it came
   down on. A wall in the way means a short throw. `GrenadeLanded`; the fuse counts down from
   landing; at 0 the grenade is removed and `explode` is called with its seed.
4. Cooldown 80 steps. `throwReadyIn` and `grenades` are in the pawn's `ThingView`.

## Fighters

- **Dive** (`FighterSystem::dive`, before the sidestep, F5): a fighter that has noticed a grenade
  (its reaction time after the throw) that will go off within the radius plus 0.6 m of it, with no
  wall already between, looks for the nearest place that is outside every blast's radius by 0.4 m or
  behind a wall, in sixteen directions at three distances, that it can reach in a straight run
  before the fuse is out (flying grenades count at their predicted landing). It runs there, drops
  its aim and does nothing else until the blast is past (`dodgeUntil`, already saved). With nowhere
  to go it lies flat (stance prone, half the damage). `PawnAction` `Dive` (`cell` where it runs,
  `target` the grenade). Anyone who is a fighter dives, from either side's grenades.
- **Throw** (`considerThrow`, every 15 steps staggered by pawn id): a fighter with grenades and a free
  arm, not mid-burst, looks at the enemies it has seen in the last 5 s and picks the spot with the most
  of them within 90% of the radius. It throws at a group of 2 or more, or at one it has no shot at
  from where it stands (hidden by cover, for the player's "grenadiers flush them out"), only if the spot
  is further than the radius plus 1.5 m from itself, has no friend (any pawn of its side, downed too)
  within the radius plus 0.5 m, and `planThrow` says the arc really lands there. It holds still
  through the wind-up.
- Molotovs and fire: once E4 is in, a second grenade kind with a fire explosion is a def and a damage
  type; nothing in the code is frag-specific.

## Numbers

All the F9 numbers are in `ExplosiveRules` and `FighterRules` (ours, tune there) and in the
grenade defs (RimWorld's, from the wiki; `unverified` lists the recalled ones: radius, fuse,
miss radius).

## Saves, hash, replays

Section `explosives` v1: per pawn `belt` (stacks by item `defName`), `thrower` (phase, steps, item,
target, aim), `knockedDown`; per grenade entity `grenade` (defs by name, position, velocity, age,
fuse, start cell, seed). All in `Sim::stateHash`. New commands `Throw` and `Explode` are in the
command log, replay files and saves by name. No format number changes (S7).

## Not done yet

- Unreal: throw and dive animations, the grenade mesh in flight (`Proj_GrenadeFrag.visual`), the
  explosion's fire, smoke, shake and Chaos debris (U21), a throw key and arc preview for the
  protagonist.
- Chemfuel and the grenade recipe; other kinds (EMP, smoke, incendiary, molotov, rockets);
  bunkers and doors open or shut (a closed door blocks the blast, an open one does not).
- Colonists who are not drafted don't dive or flee (F6's defence reactions).
- Fighters don't throw to flush a pawn they have never seen, and don't lead a moving group.
