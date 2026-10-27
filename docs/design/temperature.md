# Room temperature

Code: `ColonySim/Temperature.h`, `Private/Temperature.cpp`; the hediff side in `Health.h` /
`Private/Health.cpp` (environment hediffs), the thoughts in `Private/Mood.cpp`, the outdoor
outdoor temperature from [weather](weather.md) (`Sim::outdoorTemperature`). Tests: `Tests/TemperatureTests.cpp`,
`simrun --outdoor C` (prints a `temperature` line), CTest `simrun_temperature`. Data:
`TemperatureRules` in `data/defs/Rules/TemperatureRules.json`, `heatPusher` on the heater, cooler,
passive cooler and campfire (`Things/BuildingsPowerClimate.json`, D11), `comfyTemperatureMin/Max`
on pawn defs, the `Hypothermia`, `Heatstroke` and `Frostbite` hediffs (D7) and the `Cold` and `Hot`
thoughts (D9). Task E2. Builds on [rooms](rooms.md) and [health](health.md).

## The outdoor temperature

`Sim::outdoorTemperature()` is what the sim says the open air is: `Weather::temperatureAt` from E1
([weather.md](weather.md)), the season's mean, the day's cycle and a slow random drift. Everything
else only asks `TemperatureSystem::outdoor()`, which the sim sets at the start of every step. The
ground (the temperature under rock) is the mean of the four seasons (`Weather::yearMeanTemperature`).
`Sim::setOutdoorOverride(C)` (and `simrun --outdoor C`) holds both fixed for tests; it is not saved.

## Rooms

Every **enclosed room under a roof** (`Room::enclosed() && !outdoors()`) has a temperature. The
outdoors, a room that reaches the map edge, a huge room and any room with an unroofed cell are at
the outdoor temperature. A door cell is the mean of the rooms on its sides.

Every `intervalSeconds` (60 game seconds, ours) each room's temperature changes by

    (sum over its boundary of  G * (T_other - T)) * hours / cellCount   +   heat pushers

with the temperatures of the start of the interval, so the order rooms are visited in doesn't
matter. The boundary, per cell of the room:

| Boundary | `G` per game hour | Other side |
| --- | --- | --- |
| thin roof over a cell | 0.024 | the outdoor temperature |
| thick roof (the mountain) | 0.004 | the ground temperature |
| wall edge (a building with `building.holdsRoof`) | 0.04 | the cell beyond: its room's temperature, the outdoors, or the ground if it is rock |
| rock edge | 0.004 | the ground temperature |
| closed door | 0.06 | the cell beyond |
| open door (`Door::open`) | 6.0 | the cell beyond |
| vent (`building.vent`) | 3.0 | the cell beyond |

So a closed 5 x 5 room with a thin roof (20 wall edges, 25 roof cells) follows the outdoors with a
time constant of 25 / 1.4 = **18 game hours**; an open door brings it down to about 4 hours. A room
in rock changes over days. A wall between two rooms passes heat between them. **These conductances
are ours** (`ours` in the def): RimWorld's own equalisation constants are not on the wiki; they were
picked so a night is enough for a room to go cold and a heater can hold it. Furniture and
non-wall buildings in the way don't count.

**Heat pushers** (`heatPusher` on a building def): in RimWorld `heatPerSecond` energy per second
is shared by the room's cells. One RimWorld second is 60 ticks and a game hour is 2,500 ticks, so
one game second of a heater with 21 adds 21 x 2500 / 60 / 3600 / cells degrees: **35 degrees an hour
in a 25 cell room**. A pusher with `maxTemp` (heater 21, campfire 28) adds heat only while the room is
below it and never takes the room past it; one with `minTemp` (cooler 10, passive cooler 9) cools
only while above. Heat pushed into the open air is lost. The room is the one under the building, or
beside it. **Power (E5) decides which pushers run**: `Sim` installs
`PowerGrid::running` with `TemperatureSystem::setSourceRunning(fn(Entity) -> bool)`, so a heater or
cooler warms only while its net runs it, and a campfire (no power part) always does. Fuel (C11)
will add its test the same way.

**Fire** (E4) pushes heat with `TemperatureSystem::addHeat(cell, heatPerSecond, seconds, maxTemp)`: the same formula as a
pusher, once per fire pass, into the enclosed room the cell is in ([fire.md](fire.md)).

**Rooms change.** A new room starts at the mean temperature its cells had (cells that were
outdoors count as the outdoor temperature then), so joining two rooms mixes their heat by size and
splitting one gives each half the temperature it had. `cellTemp_` (one number per map cell, set
for cells in enclosed rooms) holds that between rebuilds. It is always the rooms' temperatures
written out again, so it isn't saved.

## Pawns

A pawn feels `ambientOf(pawn)`: the temperature at the cell it stands in. Its **comfortable range**
is the def's `stats.comfyTemperatureMin/Max` (Human 16 to 26). **What a pawn wears widens it** (C15): each worn piece's cold insulation lowers the minimum and
its heat insulation raises the maximum, through `TemperatureSystem::setComfortModifier(fn(Entity,
float& min, float& max))`, set by `Sim` from `ApparelSystem::insulation` (`apparel.md`). A pawn with
no clothes has the plain def range. The **safe range** is the comfortable one widened by 10 degrees (`safeMarginC`,
RimWorld's, recalled).

- **Cold and Hot thoughts** (mood): a situation while the pawn is outside the comfortable range.
  Staged thoughts (C18's mechanism, Mood.cpp `conditionValue`) with `situation.condition`
  `coldBelowComfort` / `hotAboveComfort`: the value is the degrees out of range, and the def's
  `coldBelowComfortThresholds` / `hotAboveComfortThresholds` ([5], ours) pick the stage, so chilly
  (-3) for the first 5 degrees and freezing (-8) beyond (D9's numbers).
- **Hypothermia and heatstroke**: every interval, a pawn outside the safe range gains severity
  (degrees outside x `severityPerDay.coldBelowComfortMinC` / `hotAboveComfortMaxC`, 0.08 a day each,
  D7's numbers), and a pawn that is not loses it (`recoverPerDay`, 0.9). They are **environment
  hediffs** in the pawn's `Health::environment`: a hediff def of class `Environment` whose `stages`
  (shivering, major, extreme, near death...) put `capacityOffsets` on Manipulation, Moving and
  Consciousness like blood loss does, so the pawn works and walks worse and goes down. At the
  def's `lethalSeverity` (1) the pawn dies of `DeathCause::Hypothermia` or `Heatstroke`
  (`Environment` for any other such hediff). `HealthSystem::setEnvironmentSeverity` marks the
  capacities for recomputing only when the stage changes. The health report lists them
  (`HealthReport::environment`).
- **Frostbite**: below `Frostbite.appliesBelowC` (-10) a pawn has `chancePerHourBelow` (0.0004) an
  hour to get a `Frostbite` injury (severity `frostbiteSeverity`, 3, ours) on a random finger or
  toe, through `HealthSystem::addInjury`. The chance uses a stream of its own (seed, pawn, time).
  Rare on purpose; real frostbite damage and amputation come with the medicine tasks.

Animals and any pawn def with `comfyTemperatureMin/Max` get the same, without the clothes stand-in.

## Unreal sees

`Snapshot::outdoorTemperature` and `RoomView::temperature` (degrees Celsius). Health reports show
the illnesses. U19 draws the temperature overlay; U13 the sky and readout.

## Saves and hashes

Section `temperature` (version 1): each enclosed room's temperature by anchor cell; rooms not
listed start at the outdoor temperature. Section `health` goes to version 2: a list of
`(hediff name, severity)` per pawn (version 1 files load with none). Both are in `Sim::stateHash`.
`TemperatureSystem` brings itself up to date with the rooms (`syncRooms`) before it saves, hashes
or answers, so a save made between steps loads back the same.

## Seeking shelter

A colonist whose cell is outside its safe range (`TemperatureSystem::outsideSafeRange`) looks for shelter, as
RimWorld's `SeekSafeTemperature` does. `JobSystem::makeShelterJob` searches outwards in rings (30 cells) for the nearest
reachable walkable cell inside the pawn's comfortable range, else inside its safe range, and starts a
`SeekShelter` job there: walk, then stand for 120 game seconds. It runs first in the needs think node (before food and
sleep), and a pawn on a waiting, hauling, recreation or gathering job that isn't forced checks every 30 seconds and drops
the job. Other work jobs run to their end and the pawn looks when the next think comes. With no such cell it does nothing
(no endless searching). Numbers in `NeedJobs.cpp` (`kNeedJobsShelter...`) are ours. Test: `ColonistTraitNeedsTests.cpp`.

## Not yet

- **Heat from fire, sunlight on rooms, cold from rain and snow**: E4 and E1. A building's own
  insulation (stone walls hold better than wood) isn't separate from the wall conductance.
- **Food rot, crops, plants dying of frost and heat stroke in animals' habits** read the room
  temperature or `outdoor()` in their own tasks (C16, D4's crop numbers).
- **Thermostat settings**: heaters and coolers use their def's limit; a per-building target is for
  the panels (U19).
