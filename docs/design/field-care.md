# Medics, firefighters and rescue under fire

Code: `ColonySim/FieldCare.h`, `Private/FieldCare.cpp` (who is on duty, what is safe, who drags whom),
`Private/FieldCareJobs.cpp` (the job system's side: duty instead of the think list, `StandBy`),
`Private/FighterRescue.cpp` (the fighter's side: walk, lift, carry, lay down). Tests: `Tests/FieldCareTests.cpp`;
ctest `simrun_medics` (`simrun --raid 300 --armed --medics --check`). Data: `Rules/FieldCareRules` (all ours). Task F10.
It stands on the alarm and defense roles ([raids.md](raids.md)), the fighter AI ([fighters.md](fighters.md)),
doctors and rescue ([medicine.md](medicine.md)) and firefighting ([fire.md](fire.md)).

The game design doc's alarm has five roles. F6 made three (Defender, Shelter, Keep working); this adds the other two,
and has the alarm's fighters drag a downed friend out of the line of fire so the medics can reach it.

| Role | What it does while the alarm is on |
| --- | --- |
| **Medic** (3) | Only Doctor work: tends the wounded and carries the downed to a bed, the medic's own Doctor priority doesn't matter. Only patients the enemies can't see, and never a fighter still on its feet (it would chase it about). With nothing to do it waits **behind the line**. |
| **Firefighter** (4) | Only Firefighter work: fires in the home area and burning colonists, only ones the enemies can't see. With nothing to do it waits behind the line. |

`SetDefenseRoleCommand{pawn, 3 or 4}` sets them; a colonist incapable of Doctor (or Firefighter) work can't take the
role ("it can't do Doctor work"). With the alarm off the roles change nothing.

## Safe ground

A cell is **safe** when no enemy of the colony that can still fight (the alarm's own rule: another faction, not a calm
wild animal, not a prisoner, not downed, not fleeing) is within `keepAway` (10 m) of it, and none within `exposureRange` (45 m) has a
line of sight from its standing eyes to a pawn standing there (`FieldCare::exposure` counts them, with the same
`Cover::lineOfSight` the fighters use). It knows where every enemy is, not only the spotted ones: "reasonably safe"
is a little generous to the colony. The list of enemies is made once a game second, at the first question.

## On duty

A colonist is **on duty** (`FieldCare::dutyWorkType`) while the alarm is on, if its role is Medic or Firefighter,
and it isn't a fighter, controlled by the player or down. The job system then skips the think list for it
(`JobSystem::dutyStep`):

1. A job it had that isn't its duty's work type ends ("alarm"). A doctor already tending keeps on.
2. With no job it asks its duty's work type only (`thinkWorkPass` with `dutyWork`), and only targets
   `FieldCare::dutyAllows` lets through: a safe cell, no fighter carrying the patient, the patient not a fighter on its
   feet. Nothing: **StandBy**, a new job type (walk to a cell, wait up to 60 game seconds, then work it out again).
   Standing by, it looks for work every 5 seconds.
3. No eating, sleeping or play meanwhile, as a called-up Defender doesn't either.

**Behind the line** (`FieldCare::rallyPoint`): `rallyBehind` (6 m) from home (the alarm's middle of the colony) on
the side away from the threat, the first safe cell in rings out to `rallySearch` (8) that it can reach and no other
pawn standing by has taken; with none safe, the first it can reach. Home itself with no threat spotted yet; where it
stands with no home.

Every `thinkEvery` (15) game seconds `FieldCare::step` looks at each colonist on duty:

- An enemy within `selfDefenseRadius` (8 m) that it can see makes it **fight**, as a Keep working colonist does
  (`enlist`, `defending`, held to 10 m of where it stood), until the alarm stops.
- A duty job whose target came under fire **before it got there** is dropped ("under fire", counted in `givenUp`):
  TendPatient before the tending starts, Rescue before the lift, ExtinguishFire at any time. A rescue already
  carrying its patient goes on. A StandBy whose place came into sight moves if there is a better place.

When the alarm stops, a StandBy ends ("off duty") and they go back to the think list.

## Rescue under fire

Every `thinkEvery` seconds, while the alarm is on and any enemy is about, `FieldCare` looks at the colony's downed
colonists that no bed holds, that no job holds (a doctor tending or rescuing it) and that no fighter is fetching. One
is **dragged** when all of this holds:

- It lies in sight of the enemy (out of sight, the medics can reach it: no drag).
- No enemy is within `dragEnemyRadius` (8 m) of it, and at most `dragMaxExposure` (2) enemies see it.
- One of the alarm's fighters (`defending`, not drafted, in Fight mode, not carrying anyone) is within `dragRadius`
  (15 m): the nearest goes.
- There is cover: in the nearest ring round the downed pawn (out to `dragDistance`, 12 cells) that has any, the safe,
  free cell nearest home that it can be carried to.
- The way is **reasonably safe**: points every `routeStep` (1.5 m) on the straight lines from the fighter to the
  downed pawn and from there to the cover are each seen by at most `dragMaxExposure` enemies.

The fighter goes into `FightMode::Rescue` (`FighterSystem::startRescue`): it stops shooting, walks to its friend
(`planGoal`, the walking the Shelter and Flee modes use), lifts it within 1.8 m (or 3 m once it has stopped as close as
it can get; else it gives up), carries it (the friend's position follows the fighter's every step, as the rescue job
does) and lays it down in the cover. `PawnAction` `LiftPawn`, then `LayPawn` with message "safe". Then it is a
fighter again, and the alarm sends it back to the line or the fight. It **lets go** (`LayPawn` "let go", the friend
where the fighter stands) if it goes down, the friend gets up, dies or is gone, the player drafts or takes it over, its
role changes or the alarm stops. The alarm doesn't move a fighter that is rescuing.

So the chain is: the fighter drags, the medic tends where it was laid, then carries it to a bed once the way there is
out of sight.

## Snapshot, saves, hash, replays

- Snapshot: `defenseRole` 3 and 4; `ThingView::rescuing` (the friend a fighter is carrying or fetching); a duty's jobs
  are ordinary jobs (`StandBy` is job type 30).
- Saves: no section of its own. The fighters section is version 3 (`rescuing`, `lifted`, `rescueTo`); defense
  plans with roles 3 and 4 and `StandBy` jobs load. Everything else follows from the alarm, roles, fighters and jobs,
  so a game saved mid-drag loads and goes on the same (test).
- Hash: the new fighter fields. Replays: nothing new (the role is `SetDefenseRole`).
- `simrun --medics` (with `--raid`) makes the first colonist the Medic and the second the Firefighter and prints
  `{"fieldCare":{tendedInFight, dragsStarted, dragged, letGo, givenUp}}`. The colony scenario gives the same two
  roles (with no raid they change nothing). F7's raid soak (`--raid-soak`, nightly and the `simrun_raid_soak` ctest)
plays every seed's raid a second time with the two roles ([raid-soak.md](raid-soak.md)).

## What a fight looks like now

`simrun --seed N --size 256 --days 0.25 --start Crashlanded --raid 300 --armed --medics` over seeds 1 to 12: in most
fights nobody goes down and the roles do nothing visible; in the hard ones (seeds 2, 3, 12) fighters drag 1 to 12
downed friends to cover and the medic tends a few during the fight. With two colonists out of the firing line the
colony shoots a little less; over the six bloodiest seeds (2, 3, 4, 6, 9, 12) as many colonists died as without the
roles (14 against 15). Some of those raids don't end within the quarter day, with or without the roles: that is F7's
balancing. The
open, flat Crashlanded ground has little cover, so many downed lie where nobody can reach them safely.

## Not done yet

- Medics don't take a downed friend they can see out of sight themselves (only fighters drag), and the fighter's way
  is checked as straight lines, not along its path.
- Drafted colonists never drag; there is no rescue order for the player.
- Raiders don't carry off their own downed.
- Keep working colonists doing Doctor or Firefighter work ignore the danger, as before.
- A carried pawn isn't drawn in its rescuer's arms (Unreal draws it where the sim puts it, the U task for fights).
