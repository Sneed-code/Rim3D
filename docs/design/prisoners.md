# Prisoners

Code: `ColonySim/Prisoners.h`, `Private/Prisoners.cpp` (the component, the numbers, capture, talks, recruiting, release),
`Private/PrisonerJobs.cpp` (the Warden scanners, the three jobs, a prisoner's day). Tests: `Tests/PrisonerTests.cpp`.
Task C22. Data: `Rules/PrisonerRules`, `Thoughts/Thoughts3.json`, the Warden work type's `tasks`.

A downed enemy is carried to a prison bed by a warden and becomes a prisoner. Wardens feed it and talk to it; what the
talk does is the prisoner's mode: wear its resistance down, recruit it once the resistance is gone, or set it free.

## Prison beds

`SetPrisonBedCommand{bed, on}` marks a bed (`PrisonBed` on the building) as a prison bed. Prison beds are for prisoners and
nobody else: `findBed` and `findPatientBed` skip them for everyone who is not a prisoner and skip every other bed for a
prisoner, and `assignBed` refuses the wrong pairing. A bed with a colonist in it can't be marked, and one with a prisoner in
it can't be unmarked. A prison cell is whatever room the bed is in, and **it must be fully enclosed**: `PrisonerSystem::cellEnclosed` is true
when the cell is in a room that is not the outdoors (walls and doors all round, roofed over every cell; a door counts as
closed). `findPrisonBed` skips every bed that fails this, so a downed enemy is not captured into an open yard. Marking a bed
is allowed anywhere (the cell may be built later). Nothing happens to a prisoner already inside if a wall is later knocked out.
Prisoners never leave their cell on their own, because nothing sends them anywhere: they sleep, rest
and stand.

## Capture

`CapturePrisoners` (Warden): a downed enemy (`PrisonerSystem::capturable`: alive, downed, on the map, another side than the
player's and than the wild animals, no animal) goes to the nearest free prison bed (`findPrisonBed`). The job is the Rescue
job's steps (`JobType::CapturePrisoner`: walk, lift, carry, lay), so the enemy's position follows the warden's. **A warden
leaves it alone while an able enemy is within `captureSafeRadius` (25 m) of it** (ours; RimWorld wardens would walk into the
fight). Raiders must be downed first, as in RimWorld; nothing is arrested.

`PrisonerSystem::imprison` at the end of the lay-down: the enemy's weapons go on the floor of the cell, it stops fighting
(`FighterSystem::standDown`) and leaves its raid squad (`RaidSystem::forget`, the `Raider` component goes), it becomes a worker
with needs (food, rest, a standard schedule) but with no Work tab row, the bed becomes its own, and a `Prisoner` component
holds its mode, resistance and the times. It keeps its `Faction` (so a recruit or a release knows where it came from, and its
old side is still its side). Event `PrisonerCaptured`.

**Nobody shoots a prisoner**: fighters (`PawnInfo::animal`), turrets and the alarm treat a pawn with `Prisoner` as nobody's
enemy; raid squads don't count it.

## Arrest

`ArrestCommand{pawn, on}` orders a pawn arrested (`on` false cancels it). It works on a colonist (not the protagonist, not a pawn
the player is walking) or a pawn of another side that is no raider (`PrisonerSystem::arrestable`); a raider has to be downed first,
and animals and prisoners are refused. The pawn gets an `ArrestOrder`. The same `CapturePrisoners` scanner then sends a warden
(`carryable` = a downed enemy, or an arrest order), with the same Rescue-job steps and the same free, closed prison bed. When the
warden lifts the pawn (`PrisonerSystem::seize`) its own job ends, it stands down and the job system leaves it alone
(`ArrestOrder::by` is the warden carrying it) until the lay-down; if the warden drops the job, the next 15 s check frees it.
At the lay-down it becomes a prisoner as in Capture, with event `PrisonerCaptured` (`message` "arrested"). A prisoner of the
player's own side starts with **resistance 0 and mode NoInteraction** (it stays locked up until the player says otherwise), cannot be
set to ReduceResistance or AttemptRecruit, and on `Release` goes back to work (its Work tab row returns) instead of walking off the map.
**Nothing here makes a colonist resist or break out**, and there is no "was imprisoned" thought beyond `Captured` and `Captive`.
Chasing a walking target gives up after the usual repaths and the scanner starts again.

## Execution

`ExecutePrisonerCommand{pawn, on}` marks a prisoner (`Prisoner::execute`). `ExecutePrisoners` (a fourth Warden scanner) sends a warden
as for a talk (`JobType::WardenInteract`; a downed prisoner is fine); after `executeWorkSeconds` (120, ours) `PrisonerSystem::execute`
kills it (`DeathCause::Executed`, "executed"), it is a prisoner no more and its bed slot is free. Event `PrisonerExecuted` (`message`
"colonist" if it was one of the colony's own). Every living colonist gets the memory `PrisonerExecuted` (-3 for 5 days) or, for
an own colonist, `ColonistExecuted` (-8 for 10 days); both are ours, `Thoughts3.json`. A prisoner with an execution ordered gets no
talks. The wardens' goodwill and faction anger (C23) are not here.

## Feeding and rest

A prisoner thinks only in `JobSystem::stepPrisoner`: sleep in its prison bed when tired (the schedule's Sleep hours as for a
colonist), lie in it while a wound is healing (`BedRest`), else wait. It **never looks for food**: `FeedPrisoners` sends a warden
with the best food the prisoner may eat (`findFoodForPrisoner`, the choice a hungry colonist makes for itself) when its food
is under `feedBelow` (0.3). `JobType::FeedPrisoner`: pick the food up, walk to the prisoner, hand it over (`feedWorkSeconds` 120,
`PawnAction::FeedPawn`); its food rises by the nutrition, `PawnAte` (`message` "fed", so no table thought). Doctors tend
prisoners (`medicalColonist`) and carry a downed prisoner to its prison bed (`needsRescue`).

## Talks

`ChatWithPrisoners` (Warden): a prisoner is due when its mode wants a talk, `chatIntervalSeconds` have passed since the last
(`Prisoner::nextChat`), it is not downed, and (Reduce Resistance) its resistance is above 0. `JobType::WardenInteract`: walk up,
talk for `chatWorkSeconds` (360, longer for a warden who can't talk well), then `PrisonerSystem::interact`. The warden gets
`chatXp` of Social.

| Mode | What a talk does |
| --- | --- |
| NoInteraction | nobody talks to it |
| ReduceResistance | resistance falls by one talk's worth, down to 0, then nobody talks to it |
| AttemptRecruit | the same; at resistance 0 each talk tries to recruit it |
| Release | the next talk sets it free (only if it can walk) |

`SetPrisonerModeCommand{pawn, mode}` sets it; a new prisoner is on ReduceResistance.

```
reduction = resistancePerChat (1.0) x opinion factor (1 until C20) x mood factor x negotiation
mood factor: 0.2 at mood 0, 1.0 at 50, 1.5 at 100 (read in between)
negotiation: (0.5 + 0.05 x Social level) x the Talking capacity           (ours)
start resistance: a random 5 to 9 (the wiki's Warrior example)
recruit chance: 0.5 x the mood factor, between 5% and 95%                 (ours)
```

RimWorld's wiki gives the first line and the mood factors; "every several hours" for the interval, the recruit chance and the
negotiation formula are ours (6 game hours; see `PrisonerRules.json`, `ours`). The roll for a recruit attempt comes from the
second and the two pawns, not the shared stream. Events `PrisonerChatted` (`message` "reduced" or "recruit failed", `amount` the
resistance left).

## Recruit, release, prison break

`recruit`: the prisoner's side becomes the player's (`FighterSystem::setFaction`), it gets a default Work tab row, its bed is let
go; event `PrisonerRecruited`. `release`: it stops being a prisoner, becomes a fleeing fighter heading for the nearest reachable
map edge and leaves the map there (`PrisonerSystem::step` finds fleeing pawns of another side with no raid squad at the edge and
calls `RaidSystem::leaveMap`); event `PrisonerReleased`. RimWorld's wardens escort the prisoner out and release gives goodwill
with its faction; **neither is here** (goodwill waits for C23).

**The prison break hook for T3**: `PrisonerSystem::breakOut(why)` frees every prisoner who can walk the way a release does
(they run for the edge; RimWorld's escapees fight) and sends a `PrisonBreak` event with `count`. `prisoners()` and `count()`
say who and how many there are. The `PrisonBreak` incident def has `needsPrisoners`.

## Thoughts

`Thoughts3.json`, all ours (RimWorld's prisoner thoughts are mostly about colonists): `Captive` (-3 while a prisoner, a situation
set by `addSituation("isPrisoner")`), `Captured` (-5 for 3 days), `WardenTalk` (+2 for a day after a talk). The prisoner's mood
feeds the two mood factors above.

## Save

Section `prisoners` (version 2): the `prisoner` part of every prisoner (mode, resistance, `capturedAt`, `nextChat`, and from
version 2 `execute`), the `prisonBed` part of every marked bed and, from version 2, the `arrest` part (`ArrestOrder::by`) of every
pawn ordered arrested. The jobs are saved in the job (`JobType` check now ends at `WardenInteract`; the three
jobs reuse `thing`, `site`, `chair`, `cell`, `count`). Commands `SetPrisonerMode`, `SetPrisonBed`, `Arrest` and `ExecutePrisoner` by name.

## Not here

Selling prisoners (C23), organ harvesting, slaves, a "was imprisoned" thought of its own, colonists who resist
arrest, wardens escorting a freed prisoner out, goodwill from a release, prisoners eating from a dispenser or
food lying in their cell, the warden's opinion in the resistance formula and prisoner traits (C20), the prisoner tab in the
panels (U18), prisoners breaking out by themselves (T3 calls `breakOut`), a prisoner's mental breaks (C19).
