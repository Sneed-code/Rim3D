# Raid strategies

Code: `ColonySim/Raids.h`, `Private/Raids.cpp` (the "Strategies" part), `CombatSystem::damageBuilding`,
the fighter rule in `Fighters.cpp` (`chooseTarget`). Tests: `Tests/RaidStrategyTests.cpp`;
`simrun --raid POINTS --raid-strategy NAME`. Data: the strategy numbers in
`data/defs/Rules/RaidRules.json`. Task F12. It builds on F6's raids ([raids.md](raids.md)); a raid is
still one squad of ordinary fighters, a strategy only changes how it arrives and what it goes for.

## One command, callable by the narrator

`StartStrategyRaidCommand{faction, points, edge, strategy, hour}` (dev command and the narrator's
way in; T1 and the incident threads call `RaidSystem::startRaid(faction, points, edge, strategy,
hour, error)` and `RaidSystem::pickStrategy(points)` directly):

- `strategy`: `Assault`, `Sappers`, `DropPods`, `Thieves`, `Kidnappers`, or `Auto`.
- `hour` 0 to 23.99 holds the raid until that hour of the day (the next one, today or tomorrow): a
  **night attack**. -1 sends it now. A waiting raid is `RaidState::pending`; when its hour comes it
  starts with the points and edge it was asked for (0 points means the colony's threat points
  *then*). If it can't start then (no way in, no loot), a `CommandFailed` event says why.
- `StartRaidCommand` is unchanged and means `Assault`, now.

**Auto** (`pickStrategy`) draws by weight (ours, `RaidRules`): Assault 60, Sappers 12, DropPods 14,
Thieves 8, Kidnappers 6. Drop pods are out under 120 points; thieves are out when nothing lies loose
on the map; kidnappers are out with no colonist. When no edge's middle can reach the colony (walls
or water shut it off), Assault, Thieves and Kidnappers are out and Sappers weigh 8 times more.

## The strategies

| Strategy | How it arrives | What it does |
| --- | --- | --- |
| **Assault** | walks in at an edge | F6's raid, unchanged |
| **Sappers** | walks in at an edge (an unreachable one if walls shut the colony off) | as Assault while it can reach the colony. When it can't, the squad flood-fills the ground it can walk on and picks the wall touching it that is nearest the target (walls, then any other blocking building with hit points), goes to the cell beside it and digs: every raider within 2.2 m of the wall takes 4 hit points a second off it (`sapDamage`, ours) through `CombatSystem::damageBuilding`. When it falls, the squad looks again and walks in, or digs the next wall. `SappersBreaking` event when a wall is picked; `SquadView::breaching` and `breach` show it |
| **DropPods** | each raider lands in a pod 8 to 24 m from the squad's target, on bare walkable ground, so inside a walled-in colony too (`DropPodLanded` event per raider) | as Assault |
| **Thieves** | walks in | each thief picks the most valuable loose item (market value times count; 0.1 each when the def has none) nobody else is going for that it can reach, walks to it, lifts up to 40 kg of the stack (`stealMass`; `ItemStolen`), and runs for the nearest edge (`FightMode::Flee`, it never shoots) and leaves the map with it, the item gone for good. A thief that goes down puts the item where it fell |
| **Kidnappers** | walks in | fights as Assault, but never shoots a downed colonist: each goes for the nearest downed colonist in 90 m (not the protagonist: losing them is game over), lifts it (`PawnKidnapped` "lifted"; the colonist gets `Kidnapped`, follows the kidnapper, can't be rescued) and runs for the edge. At the edge the colonist is **gone for good** (`PawnKidnapped` "left the map"). A kidnapper that goes down or dies puts the colonist down where it fell |

A raider that carries loot flees whatever its squad is doing. A routed squad takes its loot with it.
A colonist can still shoot a fleeing thief or kidnapper to make it drop the loot.

## Why these choices

- **Sappers need no new pawn kinds.** RimWorld gives sappers explosives; here any raider digs
  (the damage per second is ours). A pawn kind with a `sapper` flag can come with the Remnant.
- **Drop pods land out of sight of the roof rule** for now: they land on the open ground round the
  target and don't check for roofs. A roofed room is ground too, so pods can land inside a base.
- **Kidnapping needs a downed colonist**, as in RimWorld. Raiders do not down colonists on purpose
  any more than they did in F6; they take the ones the fight downs.

## Save, snapshot, hash

- Save section `raids` 2: per squad `strategy`, `hasBreach`, `breach`, `breachFrom`; the waiting raids
  (`pending`); per raider `goalThing`, `loot` and `sparesDowned`; the `kidnapped` part on a carried
  colonist. Version 1 files load with Assault squads and nothing waiting. The command is
  `StartStrategyRaid` by name. All of it is in `Sim::stateHash`.
- Snapshot: `SquadView::strategy`, `breaching`, `breach`. Events: `DropPodLanded`,
  `SappersBreaking`, `ItemStolen`, `PawnKidnapped` (appended to `EventType`).

## Not done yet

- Siege (outside the guns' range, mortars), breaching with explosives, raiders who bring and place
  their own traps and mortars; a pawn kind that sappers prefer.
- Drop pods that check for roofs and the mountain; a pod that is a thing on the map for a few
  seconds before the raiders step out.
- Kidnappers taking sleeping or prisoner colonists; thieves going for a stockpile's best stack
  rather than the best loose item; a prisoner rescue raid.
- The narrator picking strategies (T1 calls `pickStrategy`).
