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

## Aiming over a crest

An aimed shot goes at the target's chest. If that line meets ground or rock (a raider crouched behind a crest, only
its shoulders and head showing), the aim goes up 0.1 m at a time, at most to 0.15 m under the top of the body, until
the line clears (`CombatSystem` firing step, both numbers ours). In the year run's fights on hillsides half the
colonists' shots and more hit the ground in front of them (C25). Test: a skill 20 rifleman at 12 m against a pawn
crouched behind a 0.9 m crest put 0 of 34 shots into the crest and hit 28; aimed at the chest, the line ran into it.

## Cooldown bursts (sneed, 2026-11-17)

sneed found the guns too slow and jarring, so guns no longer reload. `CombatRules` `cooldownBursts`
(`shotsFactor` 3, `gapFactor` 0.5, `reloads` false) turns every gun's burst into a **store of shots**:

- **Three times the shots.** The store holds the def's `burstShotCount` times 3: autopistol 3 (it
  fired one), assault rifle 9, LMG 18, minigun 75. Guns marked `keepBurst` keep their own count:
  bolt-action and sniper rifle, charge lance, needle gun, bows, pila, the launchers, the uranium slug
  turret, the mortar and the rocketswarm, with their gaps. A one-shot gun has a store of 1.
- **Half the gap.** Shots of the store come at least `secondsBetweenBurstShots` x 0.5 steps apart
  (at least 1); a gun that used to fire one shot gets half its cooldown as the gap, so the autopistol
  fires every 15 steps (half a second), almost semi-automatic.
- **A hidden cooldown from the first shot.** The first shot from a full store starts the gun's
  cooldown (`rangedWeaponCooldown`, slowed by worn armour and tiredness, quicker in a shooting
  frenzy). Later shots don't add to it. When it runs out the store is full again, however many
  shots were fired. sneed's example: a 3-shot gun with a 2 s cooldown fires once, waits 1 s, fires
  again: the store is full 1 s later. An empty store waits for the rest of the cooldown.
- **Who fires how.** The player fires one shot per pull, and holding the trigger fires one every gap
  until the store is empty. A fighter fires all that is left in the store as one burst (after its
  aiming time, as before), then waits for the refill (`shotReadyIn` counts it, so it ducks back into
  cover).
- **No reloads.** `magazineSize` and `reloadTime` stay in the defs but are not read while `reloads`
  is false: the snapshot's `magazineSize` is 0, `Reload` is refused, and the reload paths below are
  idle.
- **Turrets** get the same shots and gaps; their cooldown counts from the burst's first shot.
- **Ranged damage is halved for now** to make up for it: `data/tuning.json` multiplies every
  projectile's `damageAmountBase` by 0.5 (bullets, arrows, the pila; explosions keep theirs).

State: `Shooter::stored` (-1 full), `refill`, `refillFrom` and `storeGun` (another weapon in the
hands starts full), saved in the `combat` section from version 5. The snapshot has `shotStore`,
`shotStoreSize`, `refillLeft` and `refillSteps` for the player's **thin yellow bar** under the gun's
name: full while the gun is full, empty after the first shot and filling over the cooldown like a
stamina bar, with a pip per shot left under it. Tests: "Cooldown bursts" in `Tests/AimingTests.cpp`.

On screen the flash and the tracer now start at the **drawn gun's muzzle** (the first-person view
gun's `muzzle` from `assets/guns.json`, else the front of the gun mesh in the pawn's hands) and fly
to where the sim's bullet lands (`UFightSubsystem::FindDrawnMuzzle`).

## Shots go where the crosshair is (sneed, 2026-11-18)

sneed aimed the bolt-action rifle at heads in the combat range and missed, and some guns hit where others missed at
other ranges. Three things were wrong:

- **The crosshair was traced against the sim's stock hitboxes**, a fixed 1.80 m man standing upright, not the body on
  screen: a short woman's head, a body leaning in a clip, an arm swinging, a tall man's crown were not where the boxes
  were, so the crosshair "passed" the drawn head and the shot went at whatever was behind it.
- **The player's spread was wide**: the gun's cone as if aimed at 25 m with RimWorld's hit chance against a whole man
  (a bolt-action rifle about 25 cm round at 30 m, a pistol much more), which is a fighter's spread, not a held sight.
- **Hitboxes were the same size on every body** while bodies are drawn at their own height (0.5 to 1.5 of 1.80 m).

Now:

- **The drawn body is the target for the player's crosshair.** Unreal traces the camera's ray against capsules from
  bone to bone of each pawn's animated mesh as it is drawn this frame (`DrawnHit`, `ASimPawnActor::TraceDrawnBody`):
  head, neck, torso (two), shoulders, upper and lower arms, hands, thighs, shins and feet, named after the sim's hitbox
  parts, radii grown with the body's height. Animals keep the sim's hitboxes for now. The nearest of that, the sim's
  ground, rock and buildings is where the crosshair is (`UFightSubsystem::ComputeAimPoint`).
- **The shot says what it was aimed at.** `ShootCommand` carries `aimPawn`, `aimPart` (`CombatSystem::hitboxPart` turns
  the label into the part index) and the spot in that pawn's frame as drawn (metres forward along its yaw, left, up from
  the ground under it). The sim aims at that spot on the pawn as the sim has it (a frame of lag in the drawing doesn't
  move it), and the bullet carries it (`DrawnAim` on `Shooter` and `Projectile`). A bullet that passes within
  `kDrawnAimRadius` (7 cm) of the spot, where the pawn stands when the bullet gets there, hits that part, whatever the
  stock hitboxes say; one that misses the spot (spread, or the target moved) still meets the stock hitboxes, other
  pawns, walls and the ground as before. Everything the sim decides stays in the sim and in the command log: replays
  and saves keep the spot (commands section 5, combat section 6), so the game stays deterministic.
- **The player's cone comes from the gun's best accuracy**, not its accuracy at 25 m: RimWorld's poor long-range numbers
  for pistols stand in for a shooter's wobble, which the player supplies with the mouse. Every gun's cone is now about
  2.4 cm round at 30 m (it was 25 cm for a bolt-action rifle and over 2 m for an autopistol), so a held sight puts the
  bullet in a head; guns differ by recoil, kick and fire rate. `CombatRules` `playerSpreadFactor` (1, ours) scales it
  for tuning. Fighters keep RimWorld's spread.
- **Hitboxes scale with the body** (`CombatSystem::hitboxScale`, PawnLooks height clamped to 0.5 to 1.5 as Unreal
  draws it): shapes, aim point and muzzle height, for bullets, blasts and melee alike.

Tests: "Crosshair" in `Tests/AimingTests.cpp`. Not yet: fighters' shots and melee still use the stock hitboxes posed
by stance only (a baked per-clip pose for them is a later step), and the drawn capsules are round where a chest is flat.

## Magazines and reloading (off while cooldown bursts are on)

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
things (grenades, arrows, rockets) are: grenades make a fighter dive instead ([explosives.md](explosives.md)). The test uses a made-up 18 m/s
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
- Diving away from grenades is F9's ([explosives.md](explosives.md)); dodging bullets by going prone is not.
- Reacting to a target that pops up from cover: the fighter AI shoots at where an enemy will
  show itself (fighters.md), so it has no separate "it just appeared" moment yet.
- Unreal: a reload animation and sound for `Reload`, a sidestep for `Dodge`, the reload key (U11).
