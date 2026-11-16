# Body health: zones, downing, blood and poise

sneed, 2026-11-16 (thread "Make combat real-time with smart AI"). Part of the real-time combat rework
([real-time-combat.md](real-time-combat.md)). It sits on top of [health.md](health.md): the body of parts, organs,
injuries, scars, infections and diseases stay as they are; what decides **downed, unconscious, bleeding and death**
for people changes. Animals keep RimWorld's rules for now.

## sneed's rules

| Rule | Number |
| --- | --- |
| Zone hit points | head 50, torso 100, each arm 50, each leg 75 |
| Downed | torso, head or a leg under 25%, or 3 or more zones under 50% |
| Unconscious | head under 25%, or blood at 1000 or less (wakes above 1000) |
| Torso-downed | crawls slowly away (fleeing) while conscious |
| Bleeding (each zone, until patched) | 75% or less: 1 a second; 50% or less: 2; 25% or less: 3 |
| Blood | 3000 (hidden). Dead at 0 |
| Patching a zone | 1 / 2 / 3 seconds (light / moderate / heavy) |
| After patching | blood comes back at 10 a second, starting 30 seconds after |
| AI and patching | patches only when safe; under 1500 blood it takes cover or runs to patch |
| Protagonist | never downed; about 100 hit points over the whole body; dies at 0 (not a pawn sneed pilots) |
| Downed and dead | can still be shot |
| Poise | base 50. An attack's force over the target's poise knocks it down; then 30 s before it can be knocked down again |
| Dismemberment | overkill on a zone of at least 50% of its base HP. Arm: 50% below the elbow, 50% the whole arm; leg: below the knee or the whole leg; torso or head: death and a gib. Force over poise + 25 on a killing blow: the whole corpse gibs. Protagonist: never |

"No active bleeding for now": no dripping effect on screen yet; the blood loss itself counts.

## How it maps onto the body

- **Zones** are subtrees of the body: the **head** zone is the neck and everything under it, each **arm** a shoulder
  and everything under it, each **leg** a leg and everything under it, the **torso** everything else (the torso and its
  organs). Listed in `HealthRules` `zones` by body part def, so another body can get zones by data.
- **Hit points**: every part in a zone has its hit points scaled so the zone's root has the zone's hit points (head
  25 to 50: x2; torso 40 to 100: x2.5; shoulder 30 to 50: x1.67; leg 30 to 75: x2.5). Organs keep their share, so a
  shot can still destroy a heart or a brain (death), as now.
- **A zone's health** is 1 minus the damage on its outside parts (inside hits hurt the outside part over them, so
  each blow counts once) over the zone's hit points. A lost part counts its full hit points.
- **Blood** is the existing blood loss (0 to 1) shown as 3000 x (1 - loss); unconscious at a loss of 2/3, dead at 1.
- Time: "a second" is a real second at 1x speed (30 sim steps).

## Phases

| Part | What | Where |
| --- | --- | --- |
| A | Zones, scaled part HP, downed / unconscious by zone, zone bleeding, blood store, regrowth after patching, protagonist 100 HP, shooting the downed and dead | sim |
| B | Self-patching (1/2/3 s a zone), AI patches when safe, under 1500 blood takes cover or runs to patch | sim |
| C | Poise and attack force: knockdown with 30 s immunity | sim |
| D | Torso-downed crawl | sim + an Unreal crawl clip |
| E | Damage numbers, layered upper-body staggers by zone and a limp after a leg hit, blood on clothes | Unreal (PC) |
| F | Dismemberment and gibs (pre-cut limbs with caps, gib chunk sets, skull and brain interior) | sim rule + Unreal art (PC) |

## Part A as built

Code: `HealthModel::buildZones`, `HealthEval::workOutZones`, the zone branch of `HealthSystem::derive` and the blood
lines of `HealthSystem::step` (`Private/Health.cpp`). Data: `HealthRules` `zones` (`Rules/Rules.json`): the bodies
that have zones (`bodies`, now only `Human`), the zone list (name, root part def, hit points) and every number in the
table above. Tests: `Tests/BodyZoneTests.cpp`. `Tests/HealthTests.cpp` takes the Human zones off and keeps checking
RimWorld's rules, which animals still use.

- **Zones**: a rule's root part def starts a zone at every part of that def (two shoulders, two legs); the rule whose
  root is the body's core part (Torso) takes what is left. Every part's hit points are scaled by its zone (torso 40 to
  100: a heart has 37.5).
- **Zone health** (`Health::zones`, `HealthSystem::zoneHealth`, by `zoneNames` order): 1 minus the damage on the
  zone's outside parts (each part at most its hit points), a lost part's full hit points, over the zone's hit points.
- **Downed** by sneed's rules only: pain shock and RimWorld's capacity thresholds no longer down a person (they still
  slow it). A vital organ destroyed or a vital capacity at 0 still kills, as before. So the Wimp trait (a lower pain shock
  threshold) and numb (resonance, takes pain away) no longer change whether a person is down; they still work for
  bodies without zones. sneed (2026-11-16) made Wimp psychological instead: see part B.
- **Unconscious** (`Health::unconscious`): head zone under 25%, or blood 1000 or less. It is a kind of downed.
- **Bleeding**: a zone at 75% or less with an untended wound that bleeds (a cut, stab or gunshot, not a bruise or
  burn) or an untended lost part in it bleeds 1, 2 or 3 blood a
  real second (`bleedPerDay` holds it as blood loss a day, so the HUD and AI keep working). Tending is "patching" until
  part B adds patching yourself in 1 to 3 seconds. RimWorld's blood loss stages (consciousness offsets) are off for a
  zoned body: blood only matters at 1000 and 0.
- **Blood comes back**: once nothing bleeds, `Health::bleedStoppedAt` (saved, health section 7) holds the step; from
  30 seconds (900 steps) on, 10 blood a second.
- **Protagonist**: never downed or unconscious; dies (`DeathCause::Wounds`, "wounds") when its zones have lost 100 hit
  points in all. Its parts are no longer doubled (that stays for an unzoned body). It still bleeds, and a destroyed
  brain or heart still kills.
- Not yet: shooting corpses (part A2: the dead have no hitboxes yet; the downed can already be shot), patching (B),
  poise (C), crawling (D).

## Part B as built

Code: `ColonySim/Patch.h`, `Private/Patch.cpp` (`PatchSystem`), `HealthSystem::patchZone` / `worstBleedingZone` /
`zoneBleedLevel`, `FighterSystem::badlyHurt`. Data: `HealthRules` `zones` `patchSeconds` [1, 2, 3] (sneed's),
`patchQuality` 0.3 and `coverBlood` 1500 (sneed's); `PatchRules` in code (`safeSeenSteps` 60, `safeRange` 30 m,
`thinkEvery` 15; ours). Tests: the "Patching" cases in `Tests/BodyZoneTests.cpp`.

- **Patching** (`Patching` component, saved in section `patch`): the pawn stands still (aim dropped, fighter
  stopped, job paused, no swings) for 1, 2 or 3 real seconds by how badly its worst zone bleeds, then that zone's
  untended bleeding wounds and lost parts are tended at quality 0.3. One zone at a time. Downed mid-patch: the patch
  is lost. `PawnAction` `Patch`: "patching" (`part` the zone, `amount` the steps) and "patched".
- **The AI** looks every half second: a person (not the player's pawn) who bleeds and is not downed, stunned,
  casting or patching patches itself when it is **safe**: not a fighter, or a fighter whose side has seen no able
  enemy within 30 m for 2 seconds.
- **Under 1500 blood** a bleeding fighter counts as badly hurt and falls back to cover (the fall-back it already had),
  where it patches once it is safe. The old rule stays too: bad pain, 30% blood lost or a bad leg send a fighter back
  (the raid soak's fighters stood in the open more without it).
- **Wimp** (sneed, 2026-11-16): a Wimp fighter hurt by an enemy falls back to cover for 12 to 20 real seconds
  (`Traits` Wimp `fleeWhenHurtSeconds`, `Fighter::cowerUntil`, saved in fighters section 4), then fights again. Not
  the player's pawn. An enemy at arm's length still makes it stand and fight.
- **The player's pawn** patches on `PatchCommand` (a key on the PC, to be bound with the stagger work). The snapshot's
  `patchZone` / `patchStepsLeft` let Unreal play a patching animation.

## Part C as built

- **Poise** (CombatRules `poise`, ours): a blow's force is its damage before armour times its damage type's force
  factor (blunt and crush 1.5, burn 0, the rest 1). Force over the pawn's poise (50) knocks it down for the blast
  knock-down's time (20 steps plus 0.6 per point of force, at most 90) and then no blow can knock it down for 900 steps
  (30 real seconds). `CombatSystem::jolt`, `Poise` component (saved in combat section 4, hashed). The event is
  `PawnKnockedDown` with message "poise". A knocked-down pawn can't swing, block or dodge.
- **Staggers** (sneed, 10:46): a hit staggers a person only if what got through armour is over 10% of the struck
  zone's hit points, and at most once per 150 steps (5 s). This replaces RimWorld's stopping-power stagger for bodies
  with zones, for bullets and melee alike; animals and mechs keep the old rule.
- **Sweeping blows**: cut, slash, blunt, crush and scratch blows sweep their arc (MeleeRules `arcHalfDegrees` 30,
  `arcRaysPerSide` 2). The aimed line is traced first; if it meets nothing, the lines either side are traced in turn
  and the first pawn met is hit. Stabs and bites only go straight. A man who steps straight back is still missed.
- **Knock-on fixes**: a person on their feet never walks slower than 40% (`zones.minMoving`; pain no longer downs, so
  it doesn't leave them crawling upright either; a raider at 3% speed held a raid open for hours). A hurt fighter
  whose side outnumbers the enemies near two to one presses on instead of backing off (a bruised trio and a lone
  colonist all backing off left a brawl with nobody coming on); bleeding under 1500 it still goes to patch. A fighter
  falling back counts cover double when it picks where to go (hurt fighters were waiting in the open far away).
- Physics wobble at the struck spot belongs to part E (Unreal).

## Part D as built

- A person downed by the torso and awake (`Health::crawls`, derived) drags itself away from the nearest able enemy
  within 20 m, one cell at a time, at 10% of its walk (`zones.crawlMoving`, ours; about 6 m in 20 s). Downed by a leg,
  by many zones, or knocked out, it lies still; with no enemy near it lies still; a rescuer or a hauler carrying it
  stops it (`PatchSystem::crawl`). The snapshot's `crawling` tells Unreal to play a crawl when it moves.

## Later (sneed, 2026-11-16)

- **Ragdolls**: unconscious pawns, corpses and severed limbs are physics ragdolls in Unreal (cosmetic: the sim keeps
  where the body is; a ragdoll that drifts is pulled back to the sim's spot).
- **Head trophies**: a severed head becomes an item that can be carried and placed as a prop (on a pike, a shelf).
  Dismemberment (part F) makes severed parts real items in the sim (def, the pawn they came from, its looks), so a
  trophy is that item plus a prop to put it on.
