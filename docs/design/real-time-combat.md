# Real-time combat: skill in the hands, sense in the head

sneed, 2026-11-16 (thread "Make combat real-time with smart AI"). Decision:
[0010](../decisions/0010-real-time-combat.md). This page is the plan; the systems it changes keep their own pages
([melee.md](melee.md), [fighters.md](fighters.md), [raids.md](raids.md), [aiming.md](aiming.md), [shields.md](shields.md),
[armour.md](armour.md)).

## What sneed asked for

Fights play like a real first/third-person action game (souls-like melee, a shooter with guns), not RimWorld's
sheet of hit and dodge chances:

1. **No dice on hits.** A blow or a bullet hits what it physically meets. A shot to the head hurts the head; armour
   that covers the whole body still only softens it (that part already works: [armour.md](armour.md)).
2. **Stats change how well you fight, not whether a roll succeeds.** Melee skill sets damage and attack speed:
   skill 0 swings 20% slower, skill 20 swings 20% faster, so the player has to react faster to a good fighter.
   A tunable rule, not a rework when it changes.
3. **Smart AI.** Shield users block. Melee fighters press hard but dodge-roll or back off. Pawns fight as a group,
   no lone suicide charges (critical for colonists; raiders may be more reckless, by a tunable profile). Anyone
   shot at goes for cover; a gunman takes cover and shoots back. Traits like Nimble become behaviour and timing.
4. **Morale.** Raiders run when half of them are down or dead, but many fight back, to the death, when cornered
   or caught while trying to get away.

## Where we start (what already works)

More of this exists than it looks from the fight demo, because the sim was built "real" from the start:

| Already real | Where |
| --- | --- |
| Bullets are traced lines against per-part hitboxes; what they meet is hit (a friend too) | `CombatSystem::trace`, [combat.md](combat.md) |
| A melee swing is timed (wind-up, commit, hit, recovery) and its strike is a traced line with real reach | `MeleeSystem`, [melee.md](melee.md) |
| Armour divides the damage of any hit wherever it lands; the part struck takes the wound | [armour.md](armour.md) |
| Held shield block (player right mouse, AI raises it at an aimed shot or wound-up swing) | [shields.md](shields.md) |
| Cover, popping up, leaning out, falling back when outnumbered, focus fire, friendly-fire checks | [fighters.md](fighters.md) |
| Dodge roll with hit-proof frames (player); sidestepping slow projectiles | [shooter-feel.md](shooter-feel.md), [aiming.md](aiming.md) |
| Squads rout at half lost; club defenders wait for friends instead of running out alone | [raids.md](raids.md), fighters.md |

**What is still RimWorld dice** (and goes):

- An AI swing's aim is thrown off by a circle sized from RimWorld's *melee hit chance* table, so a skill 0 fighter
  misses half its swings at a man standing still.
- An AI defender *rolls* RimWorld's *dodge chance* to sidestep, else rolls a *block chance* (10% to 50%).
- Melee skill does nothing to damage or speed.
- Routed raiders never fight back, even when caught.

**Guns stay as they are in principle.** A gunman's skill sets how steady its aim is (the spread cone of
[aiming.md](aiming.md)); every bullet is then a real line. That is how real shooting works (a poor shot's muzzle
wanders), so it is not a dice roll on the target: a bullet that goes into the head hits the head.

## Sim versus Unreal

Ground rule 1 holds: **the sim decides, Unreal shows.** Every swing, block, dodge, roll and shot is timed and
resolved in the sim at 30 steps a second, for the player's pawn too (Unreal sends the player's inputs as commands).
Unreal plays animations stretched to the sim's timings, adds hit-stop, flinches, sparks, ragdolls and sound.
Physics never decides a hit: a hit decided by Unreal's physics would differ on the next run and break saves,
replays and the Linux/Windows compare. The sim's hitboxes are simple capsules per body part; Unreal's animation
can lean and swing however it likes on top.

| Part | Sim | Unreal |
| --- | --- | --- |
| Swing timing (skill and armour change it) | yes | stretches the swing clip to fit |
| Strike line, reach, part hit, damage | yes | blood, sparks, hit-stop |
| AI choices: block, parry, dodge, back off, wait for friends, take cover | yes | plays them |
| Player inputs (attack, block, roll, aim, shoot) | executes them | sends them |
| Bullets, cover, line of sight | yes | tracers, impacts |
| Ragdolls, cloth, soft skin, debris | no | cosmetic only |

## How each rule works

### Melee skill: damage and speed (phase 1)

`MeleeRules` gets two lines, straight between skill 0 and skill 20 (ours, sneed's numbers):

- `skillSpeedBySkill` [0.8, 1.2]: a swing (wind-up and recovery) takes the tool's time divided by this. Skill 10
  is RimWorld's speed.
- `skillDamageBySkill` [0.8, 1.2]: damage and armour penetration of every melee blow times this.

Both stack with armour weight (heavy armour slows swings), weapon quality and stuff. Changing the rule later is
editing two numbers in `data/defs/Rules/MeleeRules.json`; making it a curve is a small code change.
Pawns without skills (animals) count as skill `defaultMeleeSkill` (4).

### No dice in melee (phase 1)

- **Where an AI swing goes**: the target's chest as it is at the commit (0.2 s before the blow), moved by a small
  wobble of the hand: 12 cm at skill 0 to 3 cm at skill 20 (`aimWobbleBySkill`, ours). The hand is a bit
  unsteady; there is no "miss 50%" any more. A swing misses because the target moved, dodged, blocked, was out of
  reach or something stood in the way.
- **Defending is a choice, not a roll.** An AI fighter reads a swing after its reaction time (14 steps at
  skill 0 to 5 at skill 20, plus or minus 4: unchanged; Nimble is 3 steps quicker, `reactionTraitSteps`).
  Read in time, it always does the sensible thing:
  - a shield on the arm: raises it (shields.md, unchanged);
  - a melee weapon: parries (the weapon up, the old block, from the front only), unless its guard is broken (below);
  - empty hands, or guard broken: sidesteps to a free side.
  Read too late (a fast weapon, a skilled attacker, a slow reader) it is hit. So skill shows as timing: a skill 20
  attacker's swing is 20% faster *and* a skill 0 defender reads it later.
- **Guard and breath** (so two parrying swordsmen don't block each other for ever): each parry costs guard; the
  third parry in a row, each within 5 seconds of the last (`guardBlocks`, `guardWindowSteps`), still stops the blow
  but breaks the guard: a long stagger (`guardBreakSteps`, 2.5 s) with no parry, so the next blow gets through
  unless it sidesteps. A sidestep needs `dodgeRecoverySteps` (1 s) before the next. A pawn
  can't defend during its own wind-up or the first `recoveryNoDefendSteps` (0.4 s) of its recovery: a whiffed or
  parried swing is the opening to punish.
- **The player** keeps defending by hand (right mouse, roll). Nothing is decided for the controlled pawn.

### Group sense and profiles (phase 1 part, phase 2 most)

A **combat profile** per side, in `FighterRules`:

| | Disciplined (colonists) | Reckless (raiders, default) |
| --- | --- | --- |
| Melee charge at an enemy | only within `ambushRange` (6 m), or to help a friend already fighting it, or on an attack order | always |
| Fights when routed and caught | n/a | yes, to the death |

Phase 1 moves the "club defenders wait" rule into the Disciplined profile and gives it to drafted colonists too
(an attack order still sends them). Later fields (phase 2): keep within a distance of friends, fall back together,
a "bot-like" profile for mechanoids, a berserker profile for manhunters.

### Cornered raiders (phase 1)

A routed raider (`FightMode::Flee`) **turns and fights to the death** (`Raider::cornered`, never routs again) when:

- it can't reach any map edge (walled in), or
- it is caught: hurt by an enemy within `caughtRange` (4 m, ours) while running, so outrunning is hopeless.

Otherwise it keeps running and never swings, as now (the ninth year pass found routed raiders beating colonists
on their way out). A cornered raider counts for the alarm again.

### Melee AI that presses but isn't stupid (phase 2)

- **Spacing**: a melee fighter steps back out of reach after its swing when the enemy is winding up and it can't
  defend (recovering), then steps in again. Spears keep their length.
- **Don't swing into a guard**: wait for the enemy's shield to drop or its stagger, or circle to its open side.
- **Dodge roll for AI**: the same roll the player has (hit-proof frames, 27 steps), used against heavy blows and
  grenades instead of the one-cell sidestep, with the roll's wait as its cost.
- **Back off when hurt** (existing fall-back rules), and **break off** to help a friend who is losing.

### Under fire: cover and shoot back (phase 3)

Fighters already do this. Missing: a *working* colonist (not called up by the alarm) who is shot at drops the job
and goes for the nearest cover away from the shooter, or shoots back if armed; paths that avoid open ground in an
enemy's sight ("danger in path costs", fighters.md "not done").

### Unreal (phase 4, needs the PC)

- Swing clips stretched to the new skill-scaled timings (they already follow `meleeStepsLeft`).
- AI parry, sidestep, guard break and roll animations; stagger flinch.
- Lock-on for the player's melee, and the player's parry window feel (already `MeleeCommand Block`).

## Phases

| Phase | What | Where | Size |
| --- | --- | --- | --- |
| 1 | Skill speed and damage; no hit-chance spread; defending by choice with guard and breath; Disciplined profile for drafted colonists; cornered raiders | sim, cloud | this thread |
| 2 | Melee spacing, waiting out guards, AI dodge roll, helping friends | sim, cloud | one thread |
| 3 | Working colonists take cover when shot at; danger-aware paths; profile fields for staying together | sim, cloud | one thread |
| 4 | Animations and feel for all of the above | Unreal, PC | one or two PC sessions |

## Phase 1 as built

Code: `MeleeSystem` (`swingSpeed`, `swingDamage`, `aimWobble`, `reactionSteps`, `guardBroken`, `prepareDefense`),
`FighterRules` `profiles`, `RaidSystem::corner` and `hurt`. Event `PawnAction` `Cornered` (`message` "walled in" or
"caught"); a `MeleeHit` "blocked" with `count` 1 broke the guard. Saves: melee section 2 (guard), raids section 4
(`cornered`). Tests: `MeleeTests` (skill numbers, no dice, damage by skill, sidesteps by reaction, parry and guard
break), `FighterTests` (drafted colonists hold, an attack order sends them), `RaidTests` (caught and walled-in
raiders). Shields keep sneed's catch chance (60/80/95% by size, shields.md); turning that into a physical cover area
is a phase 2 candidate.

## Tuning later

Every number above lives in `data/defs/Rules/MeleeRules.json` or `FighterRules.json` (and `data/tuning.json`
multipliers), so making skill matter more or less, the AI quicker or slower, or raiders braver is a data edit.
