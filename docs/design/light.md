# Light and darkness

Code: `ColonySim/Lights.h`, `Private/Lights.cpp`; the effects are in `Private/MovementSystem.cpp`
(walking) and `JobSystem::lightWorkFactor` (work). Defs: `LightRules` in `data/defs/Rules/Rules.json`,
`glower` on buildings (D11's `TorchLamp`, `StandingLamp`, `SunLamp`, `Campfire` in `BuildingsPowerClimate.json`). Tests:
`Tests/LightTests.cpp`. Picture: `simrun --size 128 --days 0 --light light.png --hour 22`. Task E3.
Builds on [rooms](rooms.md) (the roof).

## What it is

Every cell has a light level, 0 (pitch dark) to 1 (full light): the larger of the sun's light and
the lamps' light. `sim.lights().lightAt(cell)` is the one call others use.

- **Sun.** `skyGlow()` follows the hour of the day: 0 until 05:00, up to 1 at 07:00, 1 until 17:00,
  down to 0 at 19:00 (`LightRules.skyGlowByHour`, our shape; no seasons or weather yet, E1 can scale
  it). It lights only cells with no roof, thin or thick (`underOpenSky`), so a built room and a mined
  tunnel are dark inside whatever the hour.
- **Lamps.** A building def with `glower: { radius, glow }` lights the cells round its
  position: `glow` at the source, falling in a straight line to 0 at `radius` metres. Something at
  least `blockingHeightDm` tall (1.5 m: walls and rock; sandbags are lower) between the lamp and the cell casts a shadow
  (a straight line of cells from the lamp; doors don't block). Blueprints and frames give no light.
  The brightest lamp wins in a cell; they don't add up.
- **Rebuilt, not saved.** The lamp layer is worked out again when a building, a roof or the regions
  change (or after `markDirty()`), so it is neither in the save nor in the state hash. Loading a
  save and stepping once gives the same light. Cost: a rebuild is a few hundred cells per lamp.

## Darkness slows pawns

A pawn's speed follows the light in its cell (`LightRules`):

| Light | Work speed | Walking speed |
| --- | --- | --- |
| 0 | 70% | 85% |
| 0.3 | 80% | 95% |
| 0.6 and up | 100% | 100% |

Work speed multiplies mining, plant work, building and deconstructing and bill work (cooking);
eating, sleeping, hauling and fighting are not slowed. The numbers are ours (`unverified`, the
shape is RimWorld's: work is slower in the dark, the colonist panel will say "dark" under 30%).
**Without a `LightRules` def there is no night and no slowing**, which is what the unit tests with
their own small defs see. The sim starts at midnight, so a test with the real defs that counts work
steps puts a lamp beside the worker.

## For later tasks

- **C11 fuel (done):** a lamp whose building has a `Fuel` part with nothing in it is left out of
  the source scan, and `FuelSystem` calls `lights().markDirty()` when fuel runs out or comes back.
- **E5 power:** a lamp that is off should not shine. Add a power check next to the fuel one in
  `Lights::rebuild` and call `lights().markDirty()` when power changes.
- **E4 fire, U14:** `lights().sources()` lists the lamps (building, cell, radius, glow); fire can add
  its own. U14 draws a real light for each.
- **D9 thoughts:** "dark" and "in the light" can read `lightAt(pawn cell)`.
- **E5 solar:** use `skyGlow()` for the sun's output.
