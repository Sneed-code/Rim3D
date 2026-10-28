# Manned artillery: mortar and rocketswarm launcher

Code: `ColonySim/Artillery.h`, `Private/Artillery.cpp` (aiming, firing, shells), `Private/ArtilleryJobs.cpp` (the
`ManArtillery` job). Tests: `Tests/ArtilleryTests.cpp`. Data: `Things/DefenseArtillery.json` (the two guns, their
buildings and the high-explosive shell), `Projectiles/ArtilleryShells.json`, `MakeShell_HighExplosive` in
`Recipes/WeaponAndArmorRecipes.json`, the `Mortars` and `RocketswarmLauncher` projects in `Research`. It stands on
turrets ([defense.md](defense.md): phases, ammunition on the fuel system, minimum range, the blast when destroyed),
explosions ([explosives.md](explosives.md)) and fuel ([fuel.md](fuel.md)).

The rule of the game: **the sim decides where every shell lands and who it hurts, Unreal shows it.**

## Manning

A building whose def has an `artillery` block gets an `Artillery` component. It fires only while it is **manned**:

- **A colonist** ordered to it (`ArtilleryCommand` "Man") gets the `ManArtillery` job: walk to a free cell beside the
  gun (`Construction::workCell`), then stand there (`PawnAction::Man`). The job is a player order (`forced`): needs don't
  interrupt it, as RimWorld's drafted crews; it ends on "StopManning", another order, being downed, or the gun being
  destroyed or marked for deconstruction. The colony alarm leaves a crew at its post (`RaidSystem::callUp`). A drafted
  colonist is undrafted first. One colonist per gun: a second one
  ordered there takes over.
- **The player's own colonist** (the protagonist, or whoever the player controls) mans a gun it stands within `reach`
  (1.5 m, ours) of. Walking off ends it.
- Refused for anyone not of the colony (raiders, prisoners, animals), a colonist incapable of violence, one downed, or
  one the colony alarm has called up to fight.

Warm-up and cooldown count down only while the gun is manned (RimWorld's rule for the mortar).

## Aiming

`ArtilleryCommand` "Aim" picks a point, or a pawn (`target`) that the gun then follows. Refused off the map, beyond the
gun's range, inside its minimum range, or (a mortar) under a mountain's thick roof. "ClearAim" drops it, "HoldFire" and
"FireAtWill" stop and allow firing. A picked pawn that dies or leaves the map ends the aim.

- **The mortar** with no aim picks targets itself while manned: every `scanEvery` steps (10, ours) the nearest able
  enemy within its ranges, anywhere on the map (no line of sight: the shells fly over), not under a mountain. As RimWorld.
- **The rocketswarm launcher** never picks: it fires one volley at the player's aim, and the aim is used up.

## Firing and scatter

Warm-up, then a volley of `burstShotCount` shots `secondsBetweenBurstShots` apart, then the gun's cooldown (the turret
phases, `TurretPhase`). Each shot lands at a point picked evenly within the **miss radius** of the aim (RimWorld picks
a cell within it):

- Mortar: 13 m (Classic mortars) times the crew's **Mortar Miss Radius Multiplier**:
  `1 + 0.2 - 0.025 x Shooting + (1 - Sight, Sight at most 100%) - 0.5 x (Manipulation - 1)`, never under 0. Shooting 0
  gives 120% (15.6 m), 8 gives 100%, 20 gives 70% (9.1 m). From the wiki's table.
- Rocketswarm: 5.75 m whoever mans it, and each rocket of a volley tries (`spreadTries`, 8, ours) for a cell no other
  rocket of it took, so a volley covers an area.

**Mortar shells fly overhead.** A shell is an entity with an `ArtilleryShell` (not a `Projectile`): it flies
`distance / speed` steps (41 m/s: 1.37 m a step) on an arc that only Unreal needs (`artilleryShellAt`, the snapshot's
projectiles), over walls, pawns and thin roofs, and lands: under a mountain's thick roof it is stopped
(`ProjectileHit` with `Rock`, no blast), elsewhere it bursts on the ground (`ProjectileHit` `Ground`, then the
`Explosion`: 50 Bomb, 10% armor penetration, 2.9 m). A mortar under **any** roof can't fire.

**Rocketswarm rockets** are ordinary projectiles (`CombatSystem::fireFrom`) aimed straight at the ground at their
point: a wall or a pawn in the way sets them off there (24 Bomb, 36%, 2.9 m each).

## Ammunition

The fuel system ([fuel.md](fuel.md)) holds it and haulers bring it (the Refuel job; the crew stays at its post):

- Mortar: high-explosive shells, one at a time (`fuel.capacity` 1, `perUse` 1), as RimWorld's mortar holds one shell.
  Machining table, 25 steel + 15 chemfuel (Classic mortars), Crafting 4.
- Rocketswarm: 180 chemfuel, all of it for one volley (`artillery.ammoPerVolley`): it waits for a full load. It also
  needs its 150 W.

## Numbers (wiki-checked 2026-10-28)

| | Mortar | Rocketswarm launcher |
| --- | --- | --- |
| Cost | 225 steel + 6 components (Classic mortars: 75 metal + 150 steel) | 200 steel + 20 plasteel + 2 components + 180 chemfuel |
| Work, HP | 2000 ticks (2880 s), 180 HP, 28% flammable | 10000 ticks (14400 s), 200 HP, 70% |
| Range | 29.9 to 500 m | 29.9 m |
| Warm-up, volley | 240 ticks (120 steps), 1 shell | 60 ticks (30 steps), 12 rockets 10 ticks (5 steps) apart |
| Cooldown | 1680 ticks (840 steps) | 7200 ticks (3600 steps, the wiki text; its infobox says 210) |
| Blast when destroyed | 50 Bomb, 4.9 m | 50 Bomb, 6.9 m |

Times are ticks / 2 for combat (steps), ticks x 1.44 for work, as for every def. The **Classic mortars** rules are
used because RimWorld 1.6's mortar needs a reinforced barrel, which can't be made (sieges and traders bring them).

## Snapshot, events, commands, saves

- Snapshot `ThingView`: `turretPhase`, `turretTarget` (the pawn this volley is at), `artilleryCrew`, `artilleryManned`,
  `artilleryAim` with `artilleryAimX`/`Y` and `artilleryAimPawn`, `holdFire`. Shells in flight are in `projectiles`.
- Events: `ShotFired` (`thing` the gun, `def` its gun def, `target` the shell or rocket), `ProjectileHit`, `Explosion`,
  `PawnAction::Man`, `JobStarted`/`JobEnded` with `JobType::ManArtillery`.
- Command: `ArtilleryCommand{action, building, pawn, target, x, y}` ("Artillery" in replay files), actions above.
- Save section `artillery` 1: the `Artillery` part of every gun and every shell in flight (defs by name). Both are in
  `Sim::stateHash`. Saves from before it load with fresh, unmanned guns.

## Not done yet

- Raiders' sieges with their own mortars (raid-strategies.md), and the AI shooting at guns.
- Other shells (incendiary, EMP, smoke, firefoam, antigrain), choosing which shells a mortar takes, and shells cooking
  off when burnt or shot. Reinforced barrels (once sieges and traders bring them) and RimWorld 1.6's numbers.
- A thin roof does not cave in where a shell lands (rooms here roof themselves); the shell bursts below it.
- The rocketswarm launcher's "can't be reloaded during the cooldown"; its crew stays after the volley (RimWorld's pawn
  only activates it).
- Skill XP for the crew; the 4 s fuse before a badly damaged gun blows up.
- Unreal: the guns' meshes, the crew pose, the aim picker and the shell's arc and whistle.
