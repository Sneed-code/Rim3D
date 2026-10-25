# Seasons, weather and snow

Code: `ColonySim/Weather.h`, `Private/Weather.cpp`. Defs: `Weathers`, a biome's `climate` and `weather`,
and `ClimateRules` (`data/defs/Rules/ClimateRules.json`). Tests: `Tests/WeatherTests.cpp`. Report:
`simrun --seed 42 --size 128 --days 60 --weather` prints a line a day. Task E1.

`sim.weather()` answers: what season is it, how warm is it outside, what is the weather doing, how deep
is the snow in a cell. Later tasks lean on it: E2 (room temperature starts from the outdoor
temperature), E4 (rain puts fires out), C13 (frost, growing seasons), T2 (cold snap and heat wave),
U13 (the sky, rain and snow Unreal draws), S8 (biome chosen at the start).

## The year

60 days, four seasons of 15 days: spring, summer, fall, winter (`GameTime::season()`; the game starts
at the first minute of spring). `SeasonChanged` comes when a season begins.

## Outdoor temperature

`weather().outdoorTemperature()` (and `temperatureAt(second)` for any time) in degrees Celsius:

```
season's mean  +  cos cycle over the day x swing / 2  +  slow random drift
```

- **Season's mean.** The biome's `climate.seasonMeanTemperature` (temperate forest: spring 9, summer 19,
  fall 9, winter -2) sits at the middle of its season; between two middles the mean is carried smoothly
  (smoothstep), so there are no jumps at a season's edge and the game's first day, between winter and
  spring, starts at 3.5.
- **Day cycle.** `climate.dailySwing` (14 in all three biomes) is the whole range, so the amplitude is
  half. The day is coldest about 04:20 and warmest about 16:20 (`ClimateRules.dayCyclePhase`).
- **Drift.** A value between -7 and +7 that wanders over a few days (`randomVariationAmplitude`,
  `randomVariationDays`): value noise from the seed, so it needs no saved state.
- No sin or cos from the C library: the cosine is a polynomial (error under 0.002 of the swing) so
  Linux and Windows give the same bits, which the state hash needs.

With no biome the temperature is a constant 15.

## Weather

Weather kinds are `Weathers` defs. The sim starts clear. When a weather's length is up (its
`durationHours`, a random time in the range) the next is chosen: every kind the biome gives a commonality
(`weather.<kind>`) and whose `temperatureRange` holds the outdoor temperature at that moment, by
weighted random draw from the weather's own seeded stream. So snow can't start above -0.5 degrees and rain
not below 0; a weather that has begun carries on when the temperature leaves its range (wiki footnote).
`WeatherChanged` (`def`, `message` = label, `amount` = temperature) comes when the kind changes.

A change blends in over 1.6 hours (`transitionHours`): `effects()` is the old weather's numbers eased
into the new one's, and the snapshot gives `transition` 0 to 1 for Unreal to blend the sky. The dev
command `SetWeatherCommand{weather, hours}` forces a kind (the weather's usual length if hours is 0).

### What it does

Only to things **under the open sky**: a cell with no roof, thin or thick (`weather().outdoors(cell)`).

| Effect | Where | How |
| --- | --- | --- |
| Walking | `MovementSystem` | `moveSpeedFactor` of the weather times the snow's factor, on top of the light's and injuries' |
| Hit chance | `CombatSystem::spread(..., accuracyFactor)` | `accuracyFactor` of the weather at the shooter's cell multiplies the hit chance the spread is sized for |
| Sight | `FighterSystem` | a fighter spots enemies out to `sightRange` x `sightFactor` |
| Sun | `Lights::skyGlow()` | the daylight curve times the weather's `skyGlow`, so a storm is dark at noon |
| Wind generators | `PowerGrid::setWind` | `weather().wind()`, below |
| Rain, wetness, fire, lightning | `effects()` | `rainRate`, `wetness`, `extinguishesFire`, `lightning` for E4, U13 and later |

## Wind

`weather().wind()` is a factor of a wind turbine's output (E5 reads it through `setWind`, wired in `Sim`):
the weather's `windSpeedFactor` (wiki: clear 100%, fog 50%, rain 80%, storms, snow and foggy rain 150%)
times a gust that drifts between 0.25 and 1.75 over a few hours, never above 1.5 (the wiki's 0 to 150%).
It comes from the seed and the time, so it has no saved state. The gust numbers are ours. With no climate
defs it is a steady 1.0.

## Snow

A layer of depth per cell (0 to 100 %, kept in hundredths of a percent). Once an hour, on every cell
under the open sky that isn't water:

- it **gains** `snowRate x 5 %` (the weather's rate: light snow 0.8, heavy snow 1.2),
- it **loses** the melt of the temperature: nothing at or below 0 degrees, `0.058 % x T^2` from 0 to 10,
  `0.58 % x T` above (the wiki's Snow page),
- a cell that got a roof or is water loses its snow.

Snow slows walking by depth: under 3 % nothing, then 76 % from 25 %, 62 % from 50 %, 52 % from 75 %
(wiki). Pathfinding doesn't know about snow yet. `snowChanges` in the snapshot goes up when any cell
changes, so Unreal rebuilds its snow then; per cell it asks `weather().snowDepth(cell)`. The pass over a
1 km map costs about 3 ms once an hour (nothing when there is no snow and none falling).

## Numbers

| Number | Source |
| --- | --- |
| Weather commonalities (all three biomes: clear 18, fog 1, rain 2, foggy rain 1, dry and rainy thunderstorm 1 each, light and heavy snow 4 each), speed and accuracy factors, snow rates, temperature ranges | wiki Weather page and biome pages (verified for E1) |
| Snow speed by depth, melt formula | wiki Snow page |
| Seasonal means, daily swing | D4, from the wiki's biome ranges; **unverified** (the wiki has only the extremes) |
| Weather lengths (`durationHours`), `skyGlow`, `sightFactor`, day cycle phase, drift amplitude, transition length | recalled or ours, **unverified** |
| Snow gained per hour, melt applied once an hour, drift period, constant 15 with no biome | ours |

D4's guesses that E1 corrected from the wiki: the weather commonalities (D4 had made up different
numbers per biome), the movement and accuracy factors of rain, fog, storms and snow, light snow's and
heavy snow's snow rates (0.8 and 1.2), the thunderstorms' and foggy rain's temperature range (any), the
boreal and arid plant densities (0.4 and 0.24), animal densities (2.8 and 1.8) and arid disease interval
(65 days). The unused `temperatureOffset` was dropped (RimWorld's weather doesn't move the temperature;
cold snaps and heat waves are events, T2). Boreal forest's three extra weathers (overcast, blizzard,
torrential rain) and the sandstorm are not defined yet.

## Save

Section `weather` (version 1): the current and previous kind by `defName`, when the current one began
and ends, the weather's random stream, and the snow as two lists (cell index, depth) for the cells that
have any. All of it is in `Sim::stateHash`. A save from before E1 has no section and loads clear weather,
with a new weather chosen at the first step. The biome is still fixed to the temperate forest in `Sim`
(plants and weather both); S8 adds the choice.
