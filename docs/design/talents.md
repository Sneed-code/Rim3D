# Talents: the protagonist's talent tree

Code: `ColonySim/Talents.h`, `Private/Talents.cpp`. Data: `data/defs/Talents/`,
`data/defs/Rules/TalentRules.json`. Tests: `Tests/TalentTests.cpp`. Task P3. All numbers are `ours`
(RimWorld has no talent tree).

Only the protagonist (a `Colonist` with `protagonist` set) has talents. Other pawns have no state and
every stat factor for them is exactly 1.

## XP and points

The protagonist earns XP when a job ends "done" (`TalentRules.jobXp`, by job type: building 6,
bills 5, research 12, mining 3, hauling 1, ...). The n-th point costs `xpFirstPoint + xpGrowthPerPoint x
(n - 1)` (100, 125, 150, ...), up to `maxPoints` (40). Each point gives a `TalentPoint` event.
Combat XP (`TalentRules.combatXp`): 1 per shot fired (`ShotFired`), 2 per melee blow that lands on a pawn or its armor
(`MeleeHit`). Kills give nothing (the death event does not say who killed).

## Talents

A Talents def has `stat`, `perRank` (change to the stat's factor per rank), `maxRank`, `cost` (points per
rank) and optional `requires` (`{talent: rank}`). Factor = 1 + sum of `perRank x rank`, never below
`minFactor`. Stats and who asks for them:

| Stat | Used by |
| --- | --- |
| `buildSpeed` | `JobSystem::constructionSpeed` (building and deconstructing) |
| `aimSpread` | `CombatSystem` when a shot is fired (multiplies the gun's spread, not recoil) |
| `carryCapacity` | `JobSystem::carryCapacity` (so the load limit and the slowdown follow it) |
| `tradePrice` | `TradeSystem` (negotiator's price improvement) |
| `reloadTime` | `CombatSystem::startReload` (multiplies the reload steps) |
| `moveSpeed` | the movement system's speed factor (walking and running) |

Talents: FastBuilder, MasterBuilder, SteadyAim, DeadEye, PackMule, Haggler, SilverTongue, QuickHands, Gunslinger,
FleetFoot, Sprinter (the second of each pair needs rank 3 of the first).

## Commands, events, snapshot, saves

- `SpendTalentPointCommand{pawn, talent}`: one more rank. Fails for no talents, unknown talent, top rank,
  missing requirement or too few points.
- `AddTalentXpCommand{pawn, amount}`: dev command.
- Event `TalentPoint` (`thing`, `amount` = points free).
- `Snapshot::talents`: per protagonist `xp`, `xpForNextPoint`, `pointsFree`, ranks.
- Save section `talents` (version 1, format stays 20); in the state hash.
