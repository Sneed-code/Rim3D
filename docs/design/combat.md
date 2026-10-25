# Combat: guns, projectiles and hitboxes

Code: `ColonySim/Combat.h`, `Private/Combat.cpp`. Tests: `Tests/CombatTests.cpp`. Data: `Things/Weapons.json`,
`Things/Apparel.json`, `Projectiles/Bullets.json`, `Hitboxes/Hitboxes.json`, `Rules/CombatRules`,
`armorCategory` in `DamageTypes`, the projectile speed multiplier in `data/tuning.json`. Task F2.

Every shot is a real projectile. It leaves the muzzle with spread, flies with speed and drop, and
stops at the first thing it touches: the ground, rock, a building, or a pawn's hitbox. A pawn hit
goes through its armor to the body part that was hit, then into health (`HealthSystem::applyHit`,
[health.md](health.md)). **There is no hit-chance roll.** Where to stand and when to shoot is the
fighter AI ([fighters.md](fighters.md), F4); leading, reaction time, magazines and dodging are F5
([aiming.md](aiming.md)); Unreal's tracers and hit reactions are U11.

## Guns and bullets

Five guns from RimWorld's wiki with their bullets: revolver, autopistol, bolt-action rifle, pump
shotgun, assault rifle. Units follow [starting-data.md](starting-data.md): ticks become steps (/2),
real seconds become steps (x30), so a gun's `verbs.0.warmupTime` (aiming), `stats.rangedWeaponCooldown`
and `verbs.0.secondsBetweenBurstShots` are steps. `range` is metres (not used by the sim yet; the AI
will use it). Bullets: `damageAmountBase`, `armorPenetrationBase`, `stoppingPower` (not used yet:
stagger comes later) and `speed`.

**Speed.** The wiki gives bullet speed as "m/s" (revolver 55, rifles 70); the defs keep that,
converted like the importer (/30, metres per step). RimWorld's bullets are slow because it is a
top-down game; with real gravity a 55 m/s bullet fired level hits the ground after about 30 m. So
`tuning.json` multiplies `projectile.speed` by **3** (ours): rifles fly 210 m/s, pistols 165 m/s.
Drop is then about 15 cm over 37 m, and there is still a visible travel time (0.2 s across a
firefight) that leading (F5) has to deal with. Change the one number in `tuning.json` to try others.

**Gravity** is 9.81 m/s², 0.0109 m per step² (`CombatRules.gravity`). A projectile moves
`v` and falls `g/2` each step, then `vz -= g`.

## Firing

`CombatSystem::shoot(pawn, target, point, player)` (or `ShootCommand`) starts a trigger (`Shooter`):

1. **Aiming**: an aimed shot waits the gun's warm-up (`warmupTime` steps). A player shot skips it.
   During warm-up a new order only changes the aim.
2. **Firing**: `burstShotCount` shots, `secondsBetweenBurstShots` apart (assault rifle: 3, 5 steps).
   Each shot aims at the target's chest (`aimPoint`) as it is at that moment, or at the point.
3. **Cooling**: the gun's cooldown. A new order is refused until it is over (`shotReadyIn` in the
   snapshot says when). A downed shooter's shot is off.
4. **Reloading** (F5): after the shot that empties the magazine, the cooldown and then the reload
   ([aiming.md](aiming.md)). An aimed shot at a moving pawn is led (same doc).

**Drop is aimed for.** The shot goes up just enough that the arc passes through the aim point (the
low of the two ballistic solutions), like a zeroed sight. **The muzzle** is above the pawn's centre
at the stance's `muzzleHeight` (1.45 m standing, 0.95 m crouching, 0.25 m prone; ours). A building
in the shooter's own cell never stops its shots (a doorway).

## Spread (where skill comes in)

Each shot's direction is picked evenly in a circle around the aim (a cone), so missing is physical:
a shot that misses a man may still hit the man behind him, the wall, or the ground.

- **Aimed shots** get the cone that makes RimWorld's hit chance come true **against a standing man
  in the open at the aim distance**: chance = shooter accuracy ^ distance x the gun's accuracy at that
  distance, with the per-metre shooter accuracy from Shooting skill (`CombatRules.shootingAccuracyBySkill`,
  89% at 0 to 99% at 20, wiki) and the gun's touch/short/medium/long accuracy at 3/12/25/40 m, linear
  in between (RimWorld's rule; the distances are unverified). Everything else follows from the
  bullet's flight: a crouching man behind sandbags is hit less because less of him shows, a running
  one because the bullet takes time to arrive. The test "aimed shot hits a standing man as often as
  RimWorld's hit chance says" checks this to within 3% at 12 and 25 m.
- **How the cone is worked out.** At load the sim looks at a standing Human from the front through a
  2 cm grid (`CombatRules.standardTarget`) and tabulates the share of a circle of each radius around
  his chest that lands on him; a shot's cone is the radius with the wanted share. No `pow`, `log` or
  trig functions are used anywhere in combat (every platform must agree): powers are multiplied out.
- **Player shots** (`player`, the protagonist's own trigger): the cone of the gun alone, as if aimed
  at 25 m (`playerSpreadDistance`). The player aims; skill reduces recoil.
- **Recoil** (ours, `recoilPerShot` on each gun): each shot widens the cone by that slope, less
  2.5% per Shooting level (`recoilSkillReduction`), and it wears off at 0.002 per step. It matters for
  bursts and fast follow-up shots.
- Pawns without skills (raiders until F6 gives them some) shoot as level 4 (`defaultShootingSkill`).
  Sight and Manipulation do not change aim yet.

## What a projectile hits

Each step the sim follows the straight stretch the bullet flies along and takes the nearest of:

- **Ground**: the terrain surface (`Map::heightAt`) under the line, cell by cell.
- **Rock**: a rock cell stops it. No damage (mining is C3).
- **Building**: a cell whose obstacle (`fillPercent`, [cover.md](cover.md)) is above the line: a
  wall (any height) or sandbags (1.1 m) when the line is lower. The building loses the bullet's
  damage in hit points (a 300 hit point wall takes 17 rifle shots) and is destroyed at 0, which
  rebuilds regions and cover. Beds and other low furniture without `fillPercent` don't stop bullets.
- **Pawn**: a hitbox of a living pawn other than the shooter (friendly fire is real). Dead pawns
  don't stop bullets yet.
- **Gone**: off the map, or 120 steps (4 real seconds) of flight.

Ties go to the building or ground, then the lower pawn id.

## Hitboxes

`Hitboxes` defs (`HumanHitboxes`, ours) give each stance a list of shapes in the pawn's own frame
(x forward, y left, z up from the ground, metres): capsules (two points and a radius) and boxes. Each
names a body part by label: head, neck, torso, shoulders, arms (upper and lower), hands, legs (thigh
and shin), feet. Pawn defs name theirs with `race.hitboxes`. Standing is 1.8 m tall, crouching
1.1 m (leaning forward over bent legs), prone 0.4 m (lying along the facing, 1.84 m long); the tops
match the stance heights of cover.md. **Downed pawns are prone** whatever their stance. A lost
part's hitbox is gone, so a bullet passes where the arm was. `aimPoint` is each stance's chest, where
others aim. `hitboxesOf(pawn)` gives the shapes in the world for the debug view (U11).

**Facing.** `Posture` holds the stance and a facing vector. Pawns face where they walk, or where
they aim while aiming or firing.

**Which part.** A hitbox hit lands on its part or one of the outside parts below it that has no
hitbox of its own, by their share of the body (`ownOutside`): a head hitbox can hit the head, an eye,
an ear, the nose or the jaw; a hand hitbox the hand or a finger. Then, as with any hit, a bullet can go
in to an organ or bone under it (`HealthSystem::goInside`: skull, brain, heart, lungs...), and an
inside hit also hurts the outside part over it.

## Armor

RimWorld's rule, per apparel piece covering the part hit (its `apparel.bodyPartGroups` against the
part's groups), outermost layer first (`apparelLayersOuterFirst`: Overhead, Shell, Middle, OnSkin):
effective armor = armor rating (by the damage type's `armorCategory`) less the bullet's armor
penetration; a roll under half of that deflects the bullet (`ProjectileHit` with `Armor`, no
injury); under all of it halves the damage and turns a sharp hit blunt (a bruise or a crack); later
layers still use their sharp rating. Flak vest (torso and neck, 100% sharp), flak pants (legs, 55%)
and a simple helmet (upper head and ears, 45%, the steel one: RimWorld makes it from any metal and
Crashlanded's is plasteel; materials come later). Against a revolver (18%) a flak vest stops 41% of
torso hits and halves another 41%. Apparel doesn't wear down from hits yet (clothing repair is a
design goal), and the vest's and pants' movement penalty (`equippedStatOffsets`) isn't applied yet.

## Gear

`Gear` on a pawn lists the gun in its hands and its apparel; each item has `EquippedBy`. The items are
things off the map. `equip` (or `EquipCommand`) picks an item up from the map if needed and drops
what it replaces (the old gun, or apparel sharing a layer and a body part group). `DestroyThing` on a
pawn drops its gear first and refuses to destroy gear someone wears. Crashlanded's rifle, revolver,
helmet, flak vest and flak pants lie with the starting items; colonists put them on when the job
system (C1) and the fighter AI (F4) ask them to, or by `EquipCommand`.

## Events, snapshot, saves

`ShotFired` (shooter, gun, projectile, muzzle point) and `ProjectileHit` (projectile, what it hit,
where, the part and damage) are in [bridge.md](bridge.md); a pawn hit is followed by the health events.
The snapshot has each pawn's stance, facing, gun and `shotReadyIn`, and `Snapshot::projectiles`
(position and velocity, for tracers). Projectiles are entities with a `Projectile` component but no
`Thing`, so they are not in the snapshot's things or the Things events. All combat state is in the
state hash and the save (format 12). The random choices of a hit (part, armor rolls) come from a seed
each projectile gets when it is fired, so they don't depend on the order things are visited in;
triggers fire in id order from the sim's random stream.

## Not done yet

Melee (Crashlanded's knife), grenades and blast radius, stopping power, apparel damage, aim affected
by sight and manipulation, bullets
hitting corpses and plants (C2's trees should stop some bullets), shooting through doorways when
doors open and close (C5).
