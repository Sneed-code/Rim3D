# Growing zones and farming

Code: `ColonySim/Farming.h`, `Private/Farming.cpp`; the sow and harvest scanners in `Private/WorkGivers.cpp`
(`SowCropsGiver`, `HarvestCropsGiver`); `JobSystem::sowWork` in `Private/GatherJobs.cpp`. Tests:
`Tests/FarmingTests.cpp`. Data: the `CropBase` plant defs in `data/defs/Things/Plants.json` (temperatures, cold and
heat damage, `sowWork`, `sowMinSkill`), `PlantRules` in `Rules.json`. Save section `farming` 1. Task C13.
Builds on [plants](plants.md), [light](light.md), [temperature](temperature.md) and [jobs](jobs.md).

## Zones

A growing zone is a set of level 0 cells with one crop (a Things def of category Plant with `plant.sowWork`,
not a tree). Commands: `CreateGrowingZone{area, crop}` takes the cells of the area that are walkable, free of
buildings, have fertility above 0 and lie in no growing zone and no stockpile; `RemoveGrowingZoneCells{area}`;
`SetGrowingZoneCrop{zone, crop}`. A stockpile made over a growing zone skips its cells. Zone ids count up from
1 and are never reused. `sim.farming()` has the zones, `cropAt(cell)` and `stats()`; the snapshot has
`growingZones` and `growingZoneCells`.

## Sowing and harvesting (Growing work type)

- **SowCrops** (job `SowCrop`): a colonist whose Plants level is at least the crop's `sowMinSkill` walks next to
  a zone cell that has no plant of the crop and sows it: work piles up at the plant work speed until
  `sowWork`, the seedling goes in at growth 0 and `PlantSown` is sent. A wild plant in the way is cut and
  replaced (a tree is not: the cell waits for it to be felled). No sowing when the cell is too cold or hot
  for the crop (outside its `growMinTemperature` to `growMaxTemperature`) or the soil is under the crop's `fertilityMin`.
  **No sowing into the frost** (RimWorld does not sow a crop that cannot ripen before the growing season ends): in the
  open (no roof) the colonist looks at the forecast, hour by hour from now, adding up the crop's growth (the growing
  hours only, soil rate and temperature curve as in Growth below) until it is fully grown. If the temperature falls
  under the crop's `coldDamageBelow` first, or 40 days pass, the cell is not sown ("it would not ripen before the
  frost"). Only hours the clear-sky sun is bright enough count, growth is taken at `PlantRules.sowGrowthMargin` (0.7, ours: clouds are not in the forecast) of the forecast rate, and the crop needs `sowHarvestSlackHours` (36, ours) more frost-free hours after it is ripe so somebody can pick it. The forecast is the sim's own outdoor temperature curve (`Farming::setForecast`), so colonists know the
  weather ahead exactly; Under a roof the room's temperature now counts, as before. The
  answer is kept for the game hour, per crop and soil rate, so scanning a big zone stays cheap.
- **HarvestCrops** (job `HarvestPlant`, no mark needed): a crop in a zone is picked when fully grown. The yield is
  the def's `harvestYield` times the Plants skill curve (60% at 0, 100% at 8, 113% at 20), as for any picking.
  The plant is removed, so the cell is sown again.

Both give Plants experience.

## Growth

`Plants` has a growth modifier hook; `Farming::growthFactor` is installed in it. For a crop in its own zone the
hourly growth is the plant layer's (fertility and sensitivity) times:

- **temperature**, RimWorld's curve: 0 at or below `growMinTemperature` (0), rising to 1 at
  `growOptimalMinTemperature` (6), 1 up to `growOptimalMaxTemperature` (42), falling to 0 at `growMaxTemperature` (58).
  The cell's temperature is `TemperatureSystem::temperatureAt` (a greenhouse room counts);
- **light**: 0 where `lightAt` is under `growMinGlow` (0.51, `PlantRules.growthNeedsLight`). Growth happens only in the
  growing hours (06:00 to 19:00), judged at the end of each hour.

Wild plants and plants outside zones are not touched: they keep the plain growth of `plants.md`.

## Frost and heat

Every game hour a crop in a zone whose cell is below `coldDamageBelow` (0) or above `heatDamageAbove` (58) dies
with chance `degrees x PlantRules.killChancePerDegree` (0.1, ours: RimWorld damages the plant over time), drawn
from a hash of the cell and the hour, so no random stream is used. `PlantRemoved` is sent with message "frost"
or "heat"; `stats()` counts both.

## Plant spots, hydroponics and sun lamps

A building def with `plantSpot` gives its own cell a fertility (plant pot 1.0, hydroponics basin 2.8, after
RimWorld); `Plants::setFertilityOverride` asks `Farming::spotFertility`, so sowing, growth rate and `place`
all use it. A growing zone may take such cells (other buildings still keep a cell out). `plantSpot.needsPower`
(the basin, 70 W, research `Hydroponics`) stops growth and sowing while the basin has no power. Light and
temperature work as for any crop, so an indoor crop needs a roofed, heated room and a lamp.

Crops grow only 06:00 to 19:00, except where a lit **sun lamp** is near: a powered building with
`glower.growsPlants` within its `glower.plantLightRadius` (5.5 m) of the cell, and the lamps' light on the cell at
least the crop's minimum glow, makes `Plants::setGrowsAnytime` true, so the crop also grows in the night hours
(`Farming::sunLampLit`). An ordinary lamp lights the cell for sowing but gives no night growth. Tests:
`HydroponicsTests`.

## Saves

Section `farming` 1: next zone id, each zone's id, crop by `defName` and cells, the counters. Older saves
have none and load with no zones. The growth modifier is installed by the `Sim` constructor.

## Not here

Fertilizer, plants spreading, blight (T3), harvest only part of a zone, zone
filters of what may be sown by which colonist, and Unreal's zone tool (U18).
