# The bridge: commands, events, snapshots and replays

Code: `ColonySim/Commands.h`, `Events.h`, `Snapshot.h`, `Replay.h`, `Hash.h`, `Private/Bridge.cpp`,
`Private/Replay.cpp`, and the bridge part of `Sim.h`/`Sim.cpp`. Tests: `Tests/BridgeTests.cpp`,
`Tests/ReplayTests.cpp`, the `replay_record` and `replay_check` ctest runs. Task S5.

Unreal and the sim talk only through three things (architecture.md, "Sim to Unreal bridge"):

| Direction | What | Where |
| --- | --- | --- |
| Unreal to sim | **Commands**, applied at the start of the next step and logged | `Sim::submit`, `Sim::commandLog` |
| Sim to Unreal | **Events**, a list per step | `Sim::events` |
| Unreal reads sim | **Snapshot**, a read-only copy | `Sim::writeSnapshot` |

Plus two tools on top: a **state hash** (`Sim::stateHash`) and **replays** (seed plus command log).

## Commands

A `Command` is a `std::variant` of one struct per command. `Sim::submit(command)` queues it and
returns its id (1, 2, 3... in submit order). At the start of the next `step()`, before any system
runs, every queued command is applied in submit order and appended to `commandLog()` as
`LoggedCommand{step, command}`, where `step` is the game second (`time().seconds`) of that step.
A command that can't be applied changes nothing, sends a `CommandFailed` event with its id and a
reason, and is still logged.

There are four dev commands, like RimWorld's dev mode, since C1 the work and stockpile
commands (below the table, and in [jobs.md](jobs.md)), since U9 the two for direct control
([protagonist.md](protagonist.md)) and since F2 the combat commands ([combat.md](combat.md)). The
rest (place blueprint, designate, draft, actions) come with the tasks that build those systems.

| Command | Does | Fails when |
| --- | --- | --- |
| `SpawnThingCommand{def, cell, count, rot}` | `Things::spawn` of a Things def | unknown def; the thing can't go there |
| `DestroyThingCommand{thing}` | `Things::destroy`; jobs using it end first (`JobSystem::forget`), a pawn's gear is dropped | the thing doesn't exist; a pawn holds or wears it |
| `DamagePawnCommand{pawn, damageType, amount, part}` | `HealthSystem::applyDamage`; `part` -1 picks like a hit would ([health.md](health.md)) | the thing has no body; unknown damage type; no damage; the pawn is dead or the part is missing or doesn't exist |
| `TendPawnCommand{pawn, quality}` | `HealthSystem::tendAll` | the thing has no body; nothing to tend |
| `SetWorkPriorityCommand{pawn, workType, priority}` | a pawn's Work tab: 0 off, 1 to 4 | not a working pawn; unknown work type; priority outside 0 to 4; work it can never do |
| `CreateStockpileCommand{area, priority}` | a stockpile zone (ids 1, 2, 3...) | no walkable cell outside other zones in `area` |
| `RemoveZoneCellsCommand{area}` | takes cells out of their zones | no zone cell in `area` |
| `SetStockpilePriorityCommand{zone, priority}` | | unknown zone; Unstored |
| `SetStockpileAllowedCommand{zone, def, allowed}` | the zone's filter | unknown zone or def |
| `OrderHaulCommand{pawn, thing}` | "prioritize hauling": ends the pawn's job, hauls the item | not a working pawn; not an item; held by another pawn; unreachable; no room; the player is controlling the pawn |
| `ControlPawnCommand{pawn}` | the player takes over one colonist (the previous one is let go, its job ends); `kNoEntity` lets go ([protagonist.md](protagonist.md)) | not a living colonist on the map |
| `DirectMoveCommand{pawn, x, y}` | moves the controlled pawn toward (x, y) metres, at most 0.5 m a step, out of walls | not the controlled pawn; downed or dead; x or y is not a number |
| `HandsOnCommand{pawn, mode, thing, cell}` | the controlled colonist works by hand ([handson.md](handson.md)): `Work` starts the sim's job on the blueprint, frame or item `thing`, else the rock or plant in `cell` (picking up is at once); `PutDown` sets down what it carries; `Stop` ends the job | not the controlled pawn; downed or dead; out of reach; taken by another colonist; hands full; a plan with no materials; nothing to work on |
| `PlaceBlueprintCommand{def, stuff, cell, rot}` | plans a building ([construction.md](construction.md)); `stuff` is "" for a building that takes none | unknown def or stuff; can't be built; the stuff doesn't fit; the footprint is off the map, on water or rock, on a building or on another plan |
| `CancelBuildCommand{thing}` | takes back a blueprint or frame (what was delivered is put back on the ground), or unmarks a building | `thing` is none of those |
| `DeconstructCommand{thing}` | marks a finished building to be taken down | not a building; marked already |
| `AddBillCommand{building, recipe, mode, count}` | puts a bill on a work building: `mode` `Count`, `UntilHave` or `Forever` ([bills.md](bills.md)) | not a finished building; unknown recipe; the building can't make it; bad count; 15 bills already |
| `RemoveBillCommand{building, bill}` | takes a bill off | no such bill |
| `SetBillCommand{building, bill, mode, count, suspended}` | changes a bill or pauses it | no such bill; bad count |
| `FlickCommand{thing, on}` | opens or closes a power switch, or switches a consumer or generator on or off ([power.md](power.md)) | the building has no power part; a conduit or battery |
| `ThrowCommand{pawn, item, target, x, y, player}` | the pawn winds up and throws a grenade from its belt at the ground under `target` or at `(x, y)` ([explosives.md](explosives.md)); `item` "" is the first kind on the belt; `player` skips the wind-up | no such grenade; downed, knocked down, mid-shot or cooling down |
| `ExplodeCommand{x, y, radius, damage, damageType, penetration}` | dev command: a blast now ([explosives.md](explosives.md)) | unknown damage type; off the map; radius or damage not above 0 |
| `ShortCircuitCommand{thing}` | dev command and T3's hook: empties the net's batteries, destroys a conduit or switch | no power part |
| `PlaceFloorCommand{area, floor}` | lays a floor def over a rectangle at once; an empty `floor` takes floors off ([room-stats.md](room-stats.md)) | unknown or non-floor def; cell not on the map |
| `AddFilthCommand{cell, kind, layers}` | puts filth down (dev, and the hook for later sources of vomit and ash) | unknown filth kind; cell not on the map |
| `SetOutfitCommand{pawn, outfit}` | the pawn follows the outfit: it takes off what it disallows and dresses in the best it may wear ([apparel.md](apparel.md)) | not a pawn; no such outfit |
| `EditOutfitCommand{outfit, disallowed, hitPointsMin, hitPointsMax, qualityMin, qualityMax}` | changes an outfit policy | no such outfit; a bad range or unknown def |
| `AddOutfitCommand{name}` | a new outfit allowing everything | empty or taken name |
| `RemoveOutfitCommand{outfit}` | removes it; pawns on it go back to the first | no such outfit; the first one |
| `StripCommand{thing, strip}` | marks a downed or dead pawn to be stripped of clothes and weapon by a colonist with Hauling work, or takes the mark off | not a pawn on the map; not downed or dead; nothing on it |
| `SetScheduleCommand{pawn, hour, kind}` | the pawn's schedule for one hour (0 to 23): Anything, Work, Joy or Sleep ([needs.md](needs.md)) | the pawn has no schedule; bad hour |
| `DesignateCommand{area, kind}` | marks rock (`Mine`), plants (`CutPlant`) or ripe plants (`HarvestPlant`) in the rectangle for work ([mining.md](mining.md)) | `kind` is None; nothing in `area` suits |
| `CancelDesignationsCommand{area, kind}` | takes marks off, `None` for every kind, and ends the jobs on them | nothing marked in `area` |
| `AssignBedCommand{bed, pawn}` | the pawn owns the bed (and gives up its old one); `pawn` none empties the bed | not a bed; not a pawn with needs; the bed is full |
| `EquipCommand{pawn, item}` | `CombatSystem::equip`: a gun in the hands or apparel on the body ([combat.md](combat.md)); a grenade stack goes on the belt as far as it fits ([explosives.md](explosives.md)) | no body or dead; not a gun or apparel; someone else's; a job holds it |
| `ShootCommand{pawn, target, x, y, z, player}` | `CombatSystem::shoot`: aimed (warm-up, skill spread) or the player's trigger (at once, gun spread) | no gun; downed; mid-burst or cooling down; target not a pawn on the map |
| `SetStanceCommand{pawn, stance}` | standing, crouching or prone | not a pawn |
| `DraftCommand{pawn, drafted}` | drafts one of the player's pawns (its job ends, the fighter AI takes it, [fighters.md](fighters.md)) or sends it back to work | not the player's pawn on the map; downed; drafted (undrafted) already |
| `MoveOrderCommand{pawn, cell}` | a drafted pawn goes there and holds, in the best cover within 2 m | not drafted; can't stand or get there |
| `AttackOrderCommand{pawn, target}` | a drafted pawn shoots this one whenever it can; `kNoEntity` lets it choose | not drafted; target not a living pawn on the map |
| `SetFactionCommand{pawn, faction}` | dev until raids (F6): puts a pawn on a side (1 is the player's); any other side fights at once | faction 0; not a pawn |
| `SetLineageCommand{pawn, lineage}` | dev command: the pawn's looks become another lineage's (skin, hair, horns), the body stays ([looks.md](looks.md)) | the thing has no looks; unknown lineage |
| `ClimbCommand{pawn, dx, dy}` | the controlled colonist climbs the stairs or ladder at its cell whose other end lies most nearly along (dx, dy) (zero: the first); `DirectMove` waits till it is off ([levels.md](levels.md), U17) | not the colonist under direct control; downed or dead; rolling; climbing already; no link from its cell within 60 degrees of that way |
| `DodgeRollCommand{pawn, dx, dy}` | `CombatSystem::dodgeRoll`: the protagonist's roll, direction any length (zero or not a number: a backstep); start-up, a hit-proof roll, recovery, a short wait ([shooter-feel.md](shooter-feel.md)) | not a colonist under direct control; downed; prone; rolling already or waiting out the last roll |
| `CarryWeaponCommand{pawn, item}` | `CombatSystem::carryWeapon`: a weapon into the hands if empty, else on the body; the weapon in its slot is dropped ([weapons.md](weapons.md)) | no body or dead; not a weapon; someone else's |
| `SwitchWeaponCommand{pawn, slot}` | `CombatSystem::switchWeapon`: bring up the weapon carried in slot 0 Primary, 1 Sidearm or 2 Melee; no shooting or swinging for the slot's switch time | nothing in the slot or in the hands already; downed, rolling, knocked down, switching, mid-shot or mid-swing |
| `SetRefuelCommand{building, autoRefuel, fillNow}` | allow auto refuel on or off, and `fillNow` orders a fill to the top ([fuel.md](fuel.md)) | not a finished building with a fuel part |
| `StartMentalBreakCommand{pawn, breakDef}` | dev command: a mental break now, whatever the mood ([mental-breaks.md](mental-breaks.md)) | not a colonist on the map; walked by the player; downed or dead; in a break already; an unknown break; one that needs fire; one with nothing to do (no food to binge on) |
| `EndMentalBreakCommand{pawn}` | dev command: ends the break at once, no catharsis | not in a break |
| `ReloadCommand{pawn}` | `CombatSystem::reload`: a new magazine now (the protagonist's reload key, [aiming.md](aiming.md)) | no gun with a magazine; full; downed, mid-shot or reloading |
| `StartRaidCommand{faction, points, edge}` | dev until the narrator: a raid of a Factions def worth `points` (0: the colony's raid points) walks in at `edge` (0 N, 1 E, 2 S, 3 W, -1 any) ([raids.md](raids.md)) | unknown faction or edge; no way in from that edge |
| `SoundAlarmCommand{on}` | the colony alarm on (every colonist to its defense role; stays on until the player stops it) or off (back to work) | on (off) already |
| `SetDefenseRoleCommand{pawn, role, hasSpot, spot}` | a colonist's defense role: 0 Defender (with a spot to hold, or none), 1 Shelter, 2 Keep working | not the colony's colonist; a spot for another role; can't stand on the spot |
| `MarkShelterCommand{cell, clear}` | the room at `cell` is the shelter, or with `clear` none is | not a roofed room; nothing to clear |
| `MeleeCommand{pawn, target, mode}` | `Attack`: a melee swing at `target` (the controlled colonist may swing at none, straight ahead); `Block`: raises the melee weapon ([melee.md](melee.md)) | downed or dead; mid-shot; still swinging or recovering; nothing to fight with; the target isn't a living pawn in reach; no melee weapon to block with |
| `InteractCommand{initiator, recipient, interaction, outcome}` | dev (C20, [social.md](social.md)): one colonist does an Interactions def to another now; `outcome` 1 or 0 forces a romance, proposal or insult-fight roll, -1 dice | unknown interaction; not two colonists; either can't talk (down, asleep, fighting, drafted); out of earshot; needs a lover or adults |
| `RearmTrapCommand{trap}` | sets a sprung spike trap again ([defense.md](defense.md)) | not a trap; armed already; an IED, which is gone once it goes off |

In replay files a `rot` is `"North"`, `"East"`, `"South"` or `"West"` (`"North"` if left out) and an `area` is `[minX, minY, sizeX, sizeY, level]` and a storage priority its name
(`"Preferred"`; `"Normal"` if left out).

**Speed and pause are not commands.** They decide how often Unreal calls `step()`, not what a step
does, so they stay in `USimSubsystem`. A replay counts steps, so it doesn't need them.

**Adding a command** (a short checklist, so tasks running in parallel merge cleanly):

1. A struct in `Commands.h`, added at the end of the `Command` variant. Its place there doesn't
   matter to any file: saves and replays name commands (since S7, [saves.md](saves.md)).
2. Its name in `commandName` (`Bridge.cpp`), unique; `commandFromName` finds it from there.
3. Applying it in `Sim::applyCommand`.
4. Its fields in `saveCommand` (`Save.cpp`), and in `Replay.cpp` (`replayCommandJson`,
   `replayReadCommand`).
5. A row in the table above, and a test.

The `static_assert`s in the visitors stop the build if 2 or 4 (the save) is forgotten. Things are named by `Entity`; the
same commands give the same ids, so replays can name them too. Never apply a command anywhere but
`applyCommand`, and never change sim state from outside a command.

## Events

`Sim::events()` lists what happened during the last step; it is cleared when the next step
starts, so read it after every step. Before the first step it holds what setting up the map did.
`Event` is one flat struct; each type uses only its fields.

| Event | Fields | When |
| --- | --- | --- |
| `ThingSpawned` | `thing`, `def`, `cell` | a thing appears on the map (made, or put down) |
| `ThingDespawned` | `thing`, `def` | a thing leaves the map but still exists (picked up) |
| `ThingDestroyed` | `thing`, `def` | a thing stops existing; comes after `ThingDespawned` if it was on the map |
| `CommandFailed` | `command`, `message` | a command couldn't be applied |
| `PawnAction` | `thing` (the pawn), `def`, `action`, `target` | a pawn did something Unreal animates: `PickUp`, `PutDown` (U5, [pawns.md](pawns.md)), `Eat`, `LieDown`, `GetUp` (C6); `Build` and `Deconstruct` (C4) when work on `target` begins; `Cook` (C7) when a run of a bill begins at the stove `target`; `Clean` (C17) when a layer of filth at `target`'s cell starts to be cleaned; `Tend` (H1) when a doctor starts tending `target`, `LiftPawn` and `LayPawn` when a rescuer lifts the downed pawn `target` and lays it in a bed (until `LayPawn` its position follows the rescuer's); `Reload` (`target` the gun) and `Dodge` (`target` the projectile, `cell` where it steps) (F5); `Dodge` (`target` the attacker) and `Block` (`target` the attacker, none for the player's own block) against a melee swing (F8); `Roll` (P2) when a dodge roll starts: `cell` where it began, (`x`, `y`) its unit direction, `amount` the steps from start-up to the end of recovery, for stretching the animation; `Switch` (P1) when a weapon switch starts: `target` the weapon coming to the hands, `amount` the steps; `Throw` (a grenade wind-up begins) and `Dive` (`target` the grenade, `cell` where the pawn runs) (F9) |
| `PawnTended` | `thing` (the doctor), `target` (the patient), `def` (the medicine, none if by hand), `amount` (quality), `message` | a doctor finished tending ([medicine.md](medicine.md)) |
| `PawnScarred` | `thing`, `def` (Scar or OldWound), `part`, `amount` | a healed injury left a permanent scar ([medicine.md](medicine.md)) |
| `FrameStarted` | `thing` (the plan), `def` (the building), `cell` | a blueprint got its first materials and is a frame now; the plan itself came with a `ThingSpawned` |
| `OutOfFuel` | `thing` (the building), `def`, `cell` | a building's fuel reached 0 ([fuel.md](fuel.md)) |
| `Refueled` | `thing`, `def`, `cell`, `amount` (items in) | a colonist put fuel in a building |
| `RecipeDone` | `thing` (the cook), `def` (the Recipes def), `target` (the stove), `cell`, `amount` (products made), `message` | one run of a bill is done; `message` is "bill finished" when a "do X times" bill reached 0 and went ([bills.md](bills.md)) |
| `PawnInjured` | `thing`, `def` (the injury), `part`, `amount`, `cell` | a pawn got an injury (F1) |
| `PawnPartLost` | `thing`, `def` (MissingBodyPart), `part`, `amount`, `cell` | a pawn lost a body part |
| `PawnDowned`, `PawnGotUp` | `thing`, `cell` | a pawn can no longer act, or can again |
| `PawnDied` | `thing`, `message` (the cause), `cell` | a pawn died; it stays on the map with `dead` set |
| `JobStarted` | `thing` (the pawn), `def`, `job`, `target`, `cell` | a pawn took a job ([jobs.md](jobs.md)) |
| `JobEnded` | the same, and `message`: "done", "failed: why" or "interrupted: why" | a pawn's job ended |
| `PawnAte` | `thing`, `def` (the food), `amount`, `cell`, `message` ("table" or "ground") | a pawn finished eating (C6, [needs.md](needs.md)) |
| `PawnWokeUp` | `thing`, `target` (the bed or none), `amount` (its rest), `message` ("bed" or "ground") | a pawn got up from sleeping |
| `CellMined` | `cell`, `def` (the rock), `amount` (its full hit points) | a rock or ore cell was dug out and is now rough stone (C3, [mining.md](mining.md)) |
| `PlantRemoved` | `cell`, `def`, `amount` (items left), `message` ("felled" or "cut") | a plant was taken away |
| `PlantHarvested` | `cell`, `def`, `amount` (items left), `message` ("picked") | a bush was picked and goes on growing |
| `DesignationsChanged` | none | the marks changed; read `snapshot.designations` again |
| `RaidArrived` | `squad`, `message` (the Factions def), `count`, `amount` (raid points), `cell` (where it walks in) | a raid arrived (F6, [raids.md](raids.md)) |
| `SquadChanged` | `squad`, `message` (`Advance`, `Hold`, `FallBack`, `Rout`, or `gave up`), `count` | a raid squad's morale changed |
| `AlarmChanged` | `message` ("on: why", "off: why") | the colony alarm went on or off |
| `PawnLeftMap` | `thing`, `def`, `cell`, `squad` | a routed raider walked off the map; its `ThingDestroyed` follows |
| `PawnMoodBand` | `thing`, `amount` (its mood), `message` ("stable", "minor", "major" or "extreme") | a pawn's mood crossed a mental break threshold (C9, [mood.md](mood.md)) |
| `PawnMentalBreak` | `thing`, `message` (the MentalBreaks defName), `amount` (its mood) | a colonist snapped (C19, [mental-breaks.md](mental-breaks.md)) |
| `PawnMentalBreakEnded` | `thing`, `message` ("recovered", "downed", "died" or "stopped"), `amount` (seconds it lasted) | the break is over |
| `PawnInsulted` | `thing` (the one insulting), `target` | an insulting spree insulted someone |
| `PawnLeftColony` | `thing`, `message` (the break) | a colonist gave up and walked off the map; a `ThingDestroyed` follows for it and what it took |
| `ShotFired` | `thing` (shooter), `def` (gun), `target` (projectile), `x, y, z` (muzzle) | a pawn fired (F2, [combat.md](combat.md)) |
| `PowerChanged` | `thing`, `def`, `cell`, `message` ("on" or "off") | a generator, consumer or battery gained or lost power (E5, [power.md](power.md)); conduits are not announced |
| `ShortCircuit` | `thing`, `def`, `cell`, `amount` (Wd released) | a short circuit emptied a net's batteries (E5) |
| `GrenadeThrown` | `thing` (thrower), `def` (the grenade item), `target` (the grenade entity), `x, y, z` (release), `amount` (steps of flight) | a pawn released a grenade (F9, [explosives.md](explosives.md)); it is in `projectiles` from now |
| `GrenadeLanded` | `thing` (grenade entity), `def` (Projectiles def), `x, y, z`, `amount` (fuse steps) | it came to rest |
| `Explosion` | `thing` (grenade entity or none), `target` (who threw it), `def` (damage type), `amount` (radius), `x, y, z`, `cell` | a blast; the hurt pawns and broken buildings follow as their own events |
| `PawnKnockedDown` | `thing`, `amount` (steps down), `x, y, z` | a blast threw a pawn to the ground |
| `TrapSprung` | `thing` (the trap, gone for an IED), `def`, `target` (the pawn), `cell`, `x, y, z` | a pawn set off a trap (F11, [defense.md](defense.md)) |
| `TrapRearmed` | `thing`, `def`, `cell` | a sprung trap was set again |
| `ProjectileHit` | `thing` (projectile), `def` (Projectiles def), `hit`, `target`, `part`, `amount`, `x, y, z`, `cell` | a projectile stopped: ground, rock, building, pawn, armor, or gone |
| `MeleeSwing` | `thing` (attacker), `target`, `def` (melee weapon, or the pawn's def for fists), `message` (the tool), `amount` (steps to the hit), `cell` | a melee swing began (F8, [melee.md](melee.md)) |
| `WeaponSwitched` | `thing` (the pawn), `def` and `target` (the weapon now in its hands) | a weapon switch ended (P1, [weapons.md](weapons.md)) |
| `Interaction` | `thing` (talker), `target` (listener), `message` (the Interactions def), `count` (1 worked, 0 refused), `amount` (the listener's opinion after) | one colonist talked to another (C20, [social.md](social.md)) |
| `BondChanged` | `thing`, `target`, `message` (Lover, Spouse, ExLover, ExSpouse), `count` (1 formed) | a stored bond formed |
| `SocialFight` | `thing`, `target`, `message` ("started", "ended: why") | a fight with fists began or ended |
| `MeleeHit` | `thing`, `def`, `target`, `hit`, `part`, `amount`, `x, y, z`, `cell`, `message` ("hit", "deflected", "blocked", "dodged", "missed") | the hit moment of a swing |

`Things` reports its own spawns and despawns once the sim points it at the event list
(`Things::setEventLog`). Later systems add their events (shot fired, wound added, job started,
letters) here. Events say what changed; positions and states are in the snapshot.

## Snapshot

`Sim::writeSnapshot(out)` fills a `Snapshot` (reusing its memory): the game time, the map size
and a `ThingView` per thing on the map, sorted by id (`Snapshot::find` is a binary search).
A `ThingView` has the id, def, category, position (metres on a level), rotation and footprint
size for buildings, stack count for items, and hit points. Pawns with needs also have `food`, `rest` and
`asleep` ([needs.md](needs.md)), `mood` and `moodBand` ([mood.md](mood.md)), and `joy`, `comfort` and `outdoors` ([recreation.md](recreation.md)). `Snapshot::research` lists the research projects ([research.md](research.md)). Pawns with a body also have `downed` and
`dead` (F1); the rest of a pawn's health is read with `Sim::healthReport`. Things off the map (a carried item) are
not in it. Since S4 it also has the ground height under the thing (`z`), a pawn's velocity (`vx`,
`vy`, metres per step) and the route still ahead (`waypointCount` points from
`Snapshot::waypoints[firstWaypoint]`), see [movement.md](movement.md). Since C4 a `ThingView` also has a
`stage` (`Built`, `Blueprint` or `Frame`), `stuff`, `buildProgress`, `materialsHave`, `materialsNeeded` and
`markedForDeconstruct`; blueprints and frames are in the snapshot like buildings, with the size and
rotation of what they will be ([construction.md](construction.md)).
Since F2 pawns also have
`stance` (prone when downed or dead), `facingX`/`facingY`, the `weapon` in their hands and
`shotReadyIn`, and `Snapshot::projectiles` lists every bullet in flight ([combat.md](combat.md)); since F5
`rounds`, `magazineSize` and `reloading` ([aiming.md](aiming.md)); since F8 `meleePhase`, `meleeStepsLeft`,
`meleeTarget` and `blocking` ([melee.md](melee.md)); since C20 `partner` and `socialFight` ([social.md](social.md)).
Since E5 a building with a power part has `power`, `powerNet`, `powerWatts` and `batteryFill`, and the snapshot has `powerNets` ([power.md](power.md)). Since C5 a door's `ThingView` has `doorOpen` (0 to 1) and the snapshot has `rooms` (the enclosed rooms with role and
size), `roofCells` (the thin roof) and `roofChanges` ([rooms.md](rooms.md)). Since C3 the snapshot lists the `designations`
(cell and kind of every mine, cut and harvest mark; [mining.md](mining.md)).
Later tasks add what they draw and show: pawn info, the work tab.

Unreal reads only snapshots and events, never the registry or `Things.h`. That lets the sim move to
its own thread later: it will write snapshots into a buffer that Unreal swaps, with no change to
what Unreal sees.

## State hash and checkpoints

`Sim::stateHash()` is a 64-bit hash of the whole game state: time, the map's ground, the random
stream, the hauling test's lists and stats, the stockpile zones, every entity id the registry has handed out
(including the free list, which decides the next ids), and every component of every thing. Floats
are hashed by their bits. Same seed and commands give the same hash; anything else almost surely
doesn't.

It must stay cheap, because checkpoints run inside a step and S3 holds every step under about
3 ms on a 1 km map. Hashing a 1 km map's cells takes about 10 ms, so the ground (fixed after
generation) is hashed once when the sim starts, and the building layer is covered by the
buildings' footprints. A task that makes map cells change in play (mining, plants, roofs, terrain)
keeps a running hash of its layer, updated when a cell changes, and adds that.

**Every new piece of sim state must go into `stateHash()`** (and, after S6, into the save). A
missing piece makes replays look fine when they aren't.

Every `SimSettings::checkpointEvery` game seconds (default one game hour, 0 for never) the sim
records `Checkpoint{step, hash}` after the step. A replay carries them, so a run that went
differently says from roughly when.

**Regions are not hashed.** They are a cache rebuilt from the map's passage layer, and they
rebuild lazily on the first query after a change, so their ids depend on when something asked
(Unreal asking for a tooltip is enough). Nothing that changes game state may depend on region
ids or on the order regions were built in (S3: break ties by cell, not by region id). If it
does, the replay checks catch it as a mismatch.

`DefDatabase::contentHash()` fingerprints the data files (every def after inheritance and tuning,
plus the tuning values), so a replay can tell it is being played with different data.

## Replays

A `Replay` is the settings, the command log and the hashes. `recordReplay(sim)` makes one from a
running sim (queued commands not yet applied are left out); `playReplay(replay, sim, endStep)`
plays one into a fresh sim made from `replay.settings` (`ReplayFeeder` does the same one step at
a time); `checkReplay(replay, sim)` compares the hashes. Same build plus same data files gives the
same game, and CI checks that Linux (GCC) and Windows (MSVC) agree too.

Files are JSON text, one command and one checkpoint per line:

```json
{
  "format": "rim3d-replay",
  "version": 1,
  "simVersion": "0.1.0",
  "defsHash": "49e4da1eea3c345a",
  "settings": {"seed":42,"mapSize":128,"pawns":10,"items":80,"flatMap":false,"pawnDef":"Human","haulItemDef":"Steel","checkpointEvery":3600,"pathBudget":3000},
  "endStep": 86400,
  "endHash": "4abdc6ed43f4b211",
  "commands": [
    {"step":0,"type":"SpawnThing","def":"Wall","cell":[60,60,0],"count":1,"rot":"North"},
    {"step":3000,"type":"DestroyThing","thing":91}
  ],
  "checkpoints": [
    {"step": 3600, "hash": "c88a5ae35fa76262"}
  ]
}
```

Only `format`, `version` and `settings` are required; missing settings take their defaults, and
`count` and `rot` default to 1 and North. A file without hashes is a **scripted scenario**:
`Tests/replays/haul-and-build.json` is one. `version` changes when the format does; a new
`SimSettings` field must be added to `writeReplay` and `readReplay`.

**simrun:**

```
simrun --replay FILE                   plays it to its endStep (--days overrides), compares hashes
simrun --replay FILE --record OUT      also writes what this run did, with fresh hashes
simrun --seed 42 --days 1 --record OUT records a run with no commands
```

The JSON line it prints gains `commands`, `commandsFailed`, `events`, `stateHash`, `replay`
(`none`, `unchecked`, `matched` or `mismatch`), `hashesCompared` and `firstMismatch`. A mismatch
exits with code 3 and names the game second. A warning is printed if the data files or sim
version differ from the recording.

**Tests and CI.** ctest runs `replay_record` (plays the scenario with the fixture defs in
`Tests/data/`, records `build/haul-and-build.recorded.json`) and `replay_check` (plays the
recording and checks every hash). CI uploads the recording from Linux and Windows and
`tools/compare_replays.py` checks they match hash for hash.

**Bug reports from sneed's PC.** F7 in the game (U2) writes `recordReplay` to
`Saved/Replays/`; F8 (U12) writes a whole bug report folder (replay, save, readable dump, notes; see
`saves.md`). The file plus the commit it was made on reproduces the session in the cloud
with `simrun --replay`; `firstMismatch` and the checkpoints narrow down when things went wrong.

## Adding a system (checklist)

Stage 2 runs many tasks at once (`docs/plans/stage-2.md`, rule 1). A new sim system:

1. **Its own files**: `Public/ColonySim/YourSystem.h`, `Private/YourSystem.cpp`, `Tests/YourSystemTests.cpp`
   and `docs/design/your-system.md`. Helper names in anonymous namespaces get a prefix (unity builds).
2. **Wired into `Sim`** with short additions at the end of each list (members, constructor, the step
   order, `stateHash`), never reorders.
3. **Commands** by the checklist under Commands above.
4. **Events**: new `EventType` values at the end of the enum, a name in `eventTypeName`, a row in
   the events table. **Snapshot**: new fields at the end of their struct, filled in `Sim::writeSnapshot`.
5. **Saved** in a section of its own ([saves.md](saves.md), "Adding to the save"); never a new
   format number. State that is rebuilt from other state needs no section.
6. **Replays** stay the same: a change that alters what an existing replay does changes its hashes,
   which the replay checks catch. Say so in the PR if it is meant.

## Unreal side (task U2)

Code: `game/Source/Rim3D/SimSubsystem.h/.cpp` (`USimSubsystem`). Smoke test: `-Test sim`
(`SmokeTest.cpp`, `BuildSimPhases`).

- **Starting.** `StartSim(FSimStartOptions)` loads `<repo>/data` (`DefDatabase::loadDirectory`;
  `-SimData=<folder>` picks another), makes a `Sim` with those defs and takes the opening snapshot.
  Command-line options: `-SimSeed=`, `-SimMapSize=`, `-SimPawns=`, `-SimItems=`, `-SimFlat`. The
  default start is seed 42, a 256 m map, no pawns. A broken data folder is logged and the sim starts
  with no defs; a missing one is a warning. Packaged builds will need `data/` staged next to the
  game (a task for the packaging step, G1 or G2).
- **Stepping.** `Tick` adds `DeltaTime x 30 x speed` to an accumulator and runs whole steps one at a
  time (`step()`, never `stepMany`), at most 90 steps after a hitch. Speed 0 pauses. After each
  step `events()` is read and every event goes to `OnSimEvent`; a `CommandFailed` is also logged and
  shown in red on screen. Events from setting the map up (before the first step) are not sent:
  read the opening snapshot instead.
- **Snapshots and blending.** Only the last two steps of a frame are copied into `Previous` and
  `Current` (`GetPreviousSnapshot`, `GetCurrentSnapshot`). `GetBlendAlpha()` is the accumulator's
  leftover (0 to 1), so what is drawn lags the sim by less than one step but never jumps.
  `GetBlendedLocation(entity, out)` blends a thing's position between the two and returns Unreal
  centimetres; a thing that appeared this step is drawn where it is. Pausing sets alpha to 1.
- **Units.** `SimToWorld(x, y, level)` is the one place sim metres become Unreal centimetres: x and y
  times 100, Z from the ground height at that point (interpolated between cell corners, like the
  terrain mesh's vertices) plus 3 m per level, plus `WorldOrigin` (zero in the game; the smoke
  tests move it away from the sample level).
- **Commands.** `Submit(command)` queues one for the start of the next step. Nothing happens
  while paused until the next step (the period key steps once).
- **Keys** (a Slate input pre-processor, so no player controller is needed): Space pauses and
  resumes at the earlier speed, 1 / 2 / 3 set the speed, `.` steps once while paused, F7 saves a
  replay to `game/Saved/Replays/replay-<time>-day<N>.json`. Keys are ignored while the console is
  open. Console: `Sim.Speed <0-3>`, `Sim.Spawn <Def> <x> <y> [count]`, `Sim.SaveReplay`,
  `Sim.ShowTimings 0|1` (on by default), `Sim.DebugDraw 0|1` (coloured shapes at blended positions,
  until U4 and U5 draw the real things).
- **On-screen timings** (top left): game time and speed, things on the map, events so far, the
  sim's cost per frame (average and worst over about a second, red above the 5 ms budget), steps per
  frame, cost per step and per snapshot.
- Entity ids in snapshots and events are `entt::entity` (a 32-bit number with a version); they
  stay valid until `ThingDestroyed`.
- A replay file made in the game reproduces the session in the cloud only with the same commit
  and the same `data/` files; `simrun --replay FILE` says if the data differs.

## Limits for now

- The command log grows for the whole session. Fine for dev commands; when the protagonist sends
  a position every step (vanilla core input) it is about 3 MB per game day, still fine. Saves
  (S6, `saves.md`) keep the log, so a loaded session still records a full replay from step 0.
- Not thread-safe: submit, step and writeSnapshot all run on the game thread through Stage 1.
- Replays from a different build or different data files may not match; they say so instead of
  pretending.
