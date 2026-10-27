# Sky, seasons and weather on screen

Code: `game/Source/Rim3D/SkySubsystem.*` (sun, moon, atmosphere, clouds, fog, rain, snow in the air, lightning,
wet ground, the date and weather text), `SnowSubsystem.*` (snow lying on the ground), `TerrainSubsystem.*`
(`SetGroundWetness`), `Rim3DEditor/TerrainMaterialsCommandlet.cpp` (the `Wetness` input of the terrain material).
Smoke test: `sky` (`tools/smoke.ps1 -Test sky`). Task U13. Unreal side only; the sim (E1, [weather.md](weather.md))
is untouched.

**The sim decides, Unreal shows.** Everything here is read from the sim's clock and `weather()`; nothing here
changes a fight, a crop or a pawn. The random choices (where a raindrop starts, when lightning flashes) use a
fixed stream of their own, not the sim's.

## What is on screen

| What | From | How |
| --- | --- | --- |
| Sun | the sim's clock and the day of the year | a directional light that rises in the east at 06:00, is due south at noon and sets in the west at 18:00 (the sim's daylight curve in `Rules.json` ramps over 05:00 to 07:00 and 17:00 to 19:00). Its noon height follows the year: 68 degrees at midsummer (day 22.5), 28 at midwinter (day 52.5), 48 at the equinoxes. Its light is 10 lux at a clear noon (or the level's own sun's) times the daylight times the weather's `skyGlow` squared, so a storm is dark at noon |
| Moon | the same | a second directional light across the sky from the sun, up at night, bluish, 1.2 lux at full moon (about an eighth of noon; a game's moonlight, not the real hundred-thousandth), waxing and waning over 30 days |
| Sky, sunsets | the sun and the moon | the engine's sky atmosphere (blue day, red sunset, moon glow) lit by the two lights; a real-time sky light lights the ground from it. The sky light never goes below 0.1, so the night is dark but not black |
| Clouds | the weather's `skyGlow` | the engine's volumetric clouds with its simple cloud material; the weather's overcast sets the material's `Cloud_GlobalCoverage` (-0.4 clear with a few wisps, about -0.03 in rain, 0.35 in a storm) |
| Fog | the weather's `sightFactor` | exponential height fog: density `0.006 + 0.3 x (1 / sight - 1) + 0.02 x rainRate`, so fog (sight 0.5) is a mist you cannot see 50 m through and a clear day has a faint haze. The colour follows the daylight and goes whiter in snow |
| Rain | `rainRate` | up to 7,000 thin streaks in a block of air 70 m wide round where the camera looks, falling at 13 m a second, leaning with the wind, with splashes where they hit the ground. `rainRate x 1.4` of the full count is drawn (rain 0.5 draws 70%, a storm all) |
| Snow in the air | `snowRate` | up to 6,000 flakes that drift down at 1.6 m a second and sway with the wind (`snowRate / 1.2` of the full count) |
| Under a roof | `weather().outdoors(cell)` | a drop or flake whose cell has a roof (thin or the mountain's thick one) is not drawn, so the rain stops at the eaves and over the mountain |
| Lightning | `effects().lightning` (dry and rainy thunderstorms) | every 3 to 12 seconds a hard flash, a gap and a softer second flash (a white-blue directional light, 40 lux at its peak, 0.35 s). No sound yet |
| Wet ground | `rainRate` | the terrain material's `Wetness` goes up over about half a game hour of rain (to 0.45 plus the weather's `wetness`) and down over about four hours after it: darker (x0.55) and shinier (roughness x0.3) ground |
| Snow on the ground | `weather().snowDepth(cell)` | see below |
| Date and weather | the clock, `season()`, `outdoorTemperature()`, the weather def's label | "Spring 6, Year 1   12°C, rain" under the time in the overseer's top bar and in direct mode's time box (`USkySubsystem::DescribeDate` and `DescribeClimate`) |

The level's own directional light, sky light and fog are put out while the sim's sky is in use, and put back when
it is not (`Sky.Enable 0`, or no game running). A clear noon's sun takes the level's own sun's intensity if it has one.

## Snow on the ground

`USnowSubsystem` draws the sim's snow layer as a white skin one cell wide over the ground: one procedural mesh per
32 m chunk (the terrain's chunks), made only for chunks with snow and remade only when the depths in the chunk and
the cell's width round it changed. Each corner of a cell is lifted by `1.5 cm + 0.07 cm x depth%` (the mean of the four
cells that meet there), so a 100% snow is 8.5 cm deep and the edges taper. Under 8% nothing is drawn. Rock and water
cells get none, as in the sim.

The snapshot's `snowChanges` goes up when the sim's snow does (at most once an hour); then 24 chunks a frame are
looked at and a quarter of those remade, so a 1 km map takes about 40 frames and never hitches. A chunk's
mesh costs about 0.5 to 1 ms. Snow is on the ground only: not yet on plants, rock tops, roofs or buildings.

## Numbers

| Number | Source |
| --- | --- |
| Sunrise 06:00, sunset 18:00 | the sim's `skyGlowByHour` curve |
| Noon height 28 to 68 degrees over the year | ours (a latitude of about 50 degrees) |
| Sun 10 lux, moon 1.2 lux, sky light 0.1 to 1, lightning 40 lux | ours, found by eye (the project uses the engine's default exposure) |
| Fog and cloud mappings, particle counts and speeds | ours, found by eye in the `sky` smoke test's screenshots |
| Weather's `skyGlow`, `sightFactor`, `rainRate`, `snowRate`, `wetness`, `lightning` | E1's Weathers defs ([weather.md](weather.md)) |

## Console

`Sky.Enable` (0 puts the level's own sky back), `Sky.Hour` and `Sky.DayOfYear` (show a time of day and day of the
year instead of the sim's; negative follows the sim), `Sky.Precipitation` (0 draws no rain or snow),
`Sky.CloudCoverage` and `Sky.CloudDensity` (the cloud material's numbers, for tuning), `Sky.FogLuminance` (scales the
fog's own colour), `Snow.Enable`, `Snow.ChunksPerFrame`. The terrain material's `Wetness` needs the `TerrainMaterials`
commandlet run again since U13 (see [terrain.md](terrain.md)); without it the ground simply does not get wet.

## Smoke test `sky`

A 256 m map with the Crashlanded colonists, the overseer view and (for the low shots) a camera on the ground. It checks:
the noon sun's height and bearing, that the sun light points from where the sky says (summer 68 degrees, winter 28);
rain (streaks and splashes drawn, hidden over the mountain's roof, the ground soaking to wetness 0.95, haze); snow
(flakes in the air, **the number of cells drawn equal to the number of cells the sim has 8% or more of snow on**, no
chunk over 60 ms, snow still lying and up to date when the weather clears, flakes gone); a thunderstorm flashes;
fog thickens; dusk puts the sun low in the west; night has the moon up and no sun; the date text reads
"Spring 1, Year 1" and a temperature; `Sky.Enable 0` takes the sky down. Screenshots of each go to
`game/Saved/Smoke/sky/`. The sim runs ahead during a phase (up to 8 game hours of snow, 3 of rain) a little each frame.
Frame times: 200 to 280 fps at 888 x 500 on the RTX 5080, game thread 3.4 to 4.8 ms, GPU 1.5 to 3 ms.

## Not done

Stars; a thunderclap and rain sound; wind in the trees and the grass; snow on plants, rock tops, roofs and
buildings; wet buildings and pawns; drips and puddles; a sky seen from inside the mountain; Niagara versions of the
rain and snow (the streaks and flakes are instanced meshes, as in the fight effects, which keeps DLSS possible).
The sim's first day starts in spring at 3.5 degrees, so a snowfall in the smoke test melts by the afternoon; the
winter look is shown with `Sky.DayOfYear` and by the snow the sim lays on the first night.
