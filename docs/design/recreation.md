# Recreation, comfort and the outdoors

Code: `ColonySim/Recreation.h`, `Private/Recreation.cpp` (the rules and the per-second upkeep),
`Private/RecreationJobs.cpp` (choosing an activity and the `Recreate` job, part of the job system).
Tests: `Tests/RecreationTests.cpp`, the horseshoes pin in the colony scenario
([colony-scenario.md](colony-scenario.md)). Task C18. Data: [thoughts-data.md](thoughts-data.md)
(kinds, activities, thoughts), `Rules/RecreationRules.json` (the numbers this task added).

A colonist has three more needs, each 0 (empty) to 1 (full), kept in the `Joy` component (made
the first step a pawn with needs is seen, only when the data has a `JoyRules` def):

| Need | Falls | Rises | Mood (the D9 thoughts) |
| --- | --- | --- | --- |
| **Recreation** (`level`) | per hour awake and not playing: 2.5% from 30% up, 1.75% from 15%, 1% under 15%, nothing at 0 (wiki) | while playing, by the activity's rate | 0 starved -20, to 15% deprived -10, to 30% unfulfilled -5, to 70% none, to 85% satisfied +5, over 85% fully satisfied +10 (wiki) |
| **Comfort** | 0.6 a day when no furniture is in use | towards the furniture's comfort, 0.5 an hour at most | under 0.2 uncomfortable -3, over 0.7 very comfortable +3 |
| **Outdoors** | 0.5 a day indoors (sleeping too) | 3 a day outdoors | under 0.1 trapped indoors -8, under 0.3 cooped up -4, over 0.7 fresh air +2 |

New colonists start at 0.6 recreation, 0.5 comfort, 0.6 outdoors. Recreation's numbers are the wiki's
(page "Recreation"); comfort's and outdoors' rates are not on it, so they stay ours (`unverified`).

## Recreation

`RecreationActivities` (D9, widened by the recreation-types task). Each has a **kind**
(`RecreationKinds`), a joy rate an hour (the wiki's base 36%; the building's `joyGainFactor` is the
wiki's recreation power), a duration range and a place.

| Kind | Activities (building, gain factor) |
| --- | --- |
| Meditative (solitary relaxation) | walk, meditate (meditation spot), watch the clouds (day), gaze at the stars (night) |
| Social | chat at a table, relax socially (any chair, beside the others) |
| Dexterity | horseshoes (pin 1.0), billiards (table 1.3) |
| Cerebral | chess (table 1.0), poker (table 1.3, 4 players) |
| Television | watch television (tube 1.2, flatscreen 1.4, megascreen 1.6; needs power, seen from up to 3 cells) |
| Telescope | look through the telescope (1.2; the telescope must stand under the open sky) |

New buildings: `PokerTable` (50 stuff, 50 cloth, 25 wood, `ComplexFurniture`), `TubeTelevision`
(80 steel, 4 components, 200 W, `Electricity`), `FlatscreenTelevision` (140 steel, 16 components,
330 W, `Microelectronics`), `MegascreenTelevision` (450 W, not on the build menu: RimWorld gives it
as a quest reward), `Telescope` (ours: buildable, 50 steel and a component, `Electricity`) and
`MeditationSpot` (free). Costs and sizes are the wiki's; build work and hit points of the new
television sets and the telescope are ours. Activity flags added: `nightOnly`, `onBuilding` (stand
on the building's cell), `openSky`, `chairs` (sit in any chair; the first sitter's chair names the
group, later ones sit within 5 m of the nearest mate and share its `thing`, so C20's table chat
applies), `reach` (cells from the building a place may be).

- **Tolerance.** Each pawn has a tolerance per kind, 0 to 1. A joy hour of an activity gives
  `joyPerHour x max(0.1, 1 - tolerance of its kind) x the building's joyGainFactor`. Every second
  of play adds two thirds of the joy gained to the kind's tolerance (wiki); it wears off 10% a day
  (the wiki's moderate-wealth rate; the wealth steps of 7% to 18% are not used yet).
- **Boredom (variety).** A kind above 50% tolerance is *boring* until it is under 30%
  (`JoyTolerance::bored`, `Joy::boredWith`, saved). A pawn picks among kinds it is not bored of
  first, and only when none can be done now goes back to a boring one. So a colony needs several
  kinds of joy buildings: a pawn with only horseshoes plays them, gets less and less, then walks.

- **Choosing** (`JobSystem::makeRecreationJob`): every activity of a kind the pawn is not bored of that it can do now, the most joy an
  hour wins, the nearest on a tie, then the first by name. Nothing is random.
- **When.** In a **Joy hour** (the standard schedule has Joy from 19:00 to 21:59, three evening
  hours; `JoyRules.joyHours`, inclusive) a pawn that is not hungry or tired plays until its
  recreation reaches 0.95, one activity after another. In an **Anything hour** a pawn with nothing
  to work on plays when its recreation is under 0.35 (work comes first; the think list is needs,
  schedule, work, joy, idle). **Work and Sleep hours** are for work and sleep: a Work hour ends a
  game ("interrupted: time to work"), a hungry or tired pawn drops it as it drops hauling.
  Pawns the player controls and fighters don't think, so they never play.
- **The job** (`JobType::Recreate`, two steps: walk to the spot, play). `activity` is the def,
  `thing` the building (none for a walk or watching the clouds), `cell` where the pawn stands,
  `chair` the chair it sits in, `count` 1 when it strolls, `until` the second it ends. The cell
  (or chair cell) is reserved. It ends after a random half hour to an hour and a half (by the
  activity; the length comes from a hash of pawn and time, not the sim's random stream) or at full
  recreation. Joy rises inside the job, every second.
- **Places.**
  - *Horseshoes, chess, billiards*: a free walkable cell next to the building, within 40 m. A
    building takes at most the smaller of the activity's `users[1]` and its
    `building.maxSimultaneousUsers`. (`standCells` in the defs are not used: any free neighbouring
    cell will do. `users[0]` is not used either: a lone player plays at full rate, as RimWorld's
    pawns play chess alone.)
  - *Chat at a table*: a chair next to any table (the same chair rule as eating), up to six at a
    table. Since C20 they talk while they sit: pawns beside each other at a table or a game start talks every 15 minutes or so ([social.md](social.md)).
  - *A walk*: strolls from place to place over open ground, each leg 4 to 18 m (integer offsets
    from the hash). *Watching the clouds*: the nearest open-air cell, from 06:00 to 18:59 only.
    "Open air" is `!Rooms::indoors`. The `weather: Clear` condition is not checked until the
    weather exists (E1).

## Comfort

What counts as furniture in use (`RecreationSystem::comfortInUse`): a bed while sleeping (its
`stats.comfort`, 0.6 if the def has none), the ground while sleeping on it (0.1), a chair while
eating at a table or chatting (`stats.comfort`: stool 0.5, armchair 0.8). The need moves towards
that number, up at 0.5 an hour at most, down four times as fast as it falls unused. Beds and
chairs already carry `stats.comfort` from C4 and D11.

## Outdoors

`Rooms::indoors(cell)` of the pawn's cell: under a roof in an enclosed room. A pawn that works
or idles under the sky stays high; one asleep or idling in a bedroom drifts down.

## What Unreal sees

- **Snapshot:** a pawn with a `Joy` has `joy`, `comfort` and `outdoors` (else -1).
- **Job:** `JobType::Recreate` in `JobStarted`/`JobEnded` and the snapshot (`jobTarget` the
  building). Unreal can read `Joy` through the snapshot only; there is no new event or command.
  The schedule's Joy hours are set with the existing `SetScheduleCommand`.
- **Thoughts:** `sim.mood().thoughts(pawn)` lists the stage the need is in, e.g. "bored stiff".
  The mood system reads **staged situations** generically: a thought def with
  `situation.condition`, `stages` and a list named `<condition minus Need>Thresholds`
  (`MoodSystem::conditionValue` knows `joyNeed`, `comfortNeed`, `outdoorsNeed`; the next task
  with a staged thought adds its case there).

## Save

Section `recreation` (version 2: tolerance entries carry `bored`; version 1 loads with it off): per pawn with a `Joy`, its three needs, its tolerances by kind
`defName`, and the activity of its recreation job by `defName`. Older saves have no such section
and load without it; the needs are made fresh on the first step. Hashed in `Sim::stateHash`.

## Not yet

- Weather for the clouds (E1), a bored pawn's mental breaks (C19),
  joy from other sources (a drink, a meal that tastes good, art), reading, music, the aurora's five times joy for skygazing, tolerance decay by colony wealth, the audience area and facing of a television, the
  recreation policy per pawn, rooms with joy buildings counting for impressiveness (C17), the
  "recreation power" of outdoor spots, an animation or pose for playing (U task).
