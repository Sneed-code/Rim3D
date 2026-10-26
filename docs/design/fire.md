# Fire and firefighting

Code: `ColonySim/Fire.h`, `Private/Fire.cpp` (the layer), `Private/FireJobs.cpp` (the job),
`ExtinguishFiresGiver` in `Private/WorkGivers.cpp`, `TemperatureSystem::addHeat`. Tests:
`Tests/FireTests.cpp`, ctest `simrun_fire` (`simrun --start Crashlanded --fire --check`). Data:
`FireRules` in `data/defs/Rules/FireRules.json`, `stats.flammability` on Things and floors,
`stuffProps.statFactors.flammability` on stuff, the `Firefighter` work type (D-tasks). Task E4.
Builds on [weather](weather.md), [temperature](temperature.md), [rooms](rooms.md), [plants](plants.md),
[room-stats](room-stats.md) (the ash), [health](health.md) and [jobs](jobs.md).

## The layer

Fire is a sparse map from cell to `FireCell {size, age, plantDamage}`. Only cells that burn cost
anything: a pass runs every `passSeconds` (60 game seconds, ours) over the fires in cell order, and
a colony with nothing alight pays for one empty check a minute. All rates are RimWorld *per tick*
numbers (1 tick is 1.44 game seconds) times the ticks in a pass.

**Flammability of a cell** (`Fire::flammabilityAt`) is the largest of: its building (the def's
`stats.flammability` times the `BuiltOf` stuff's flammability factor), each item lying there, the
plant, and the floor (`flammability` in the Terrain def: wood floor and carpet). Stone, steel and
bare ground are 0 and never catch. Grass is 1.3, trees 0.8, wood 1.0.

**Starting.** A fire starts from `Fire::ignite(cell, size, force)`, which fails for water, rock, a
cell already burning, a wet cell (below) and a cell with nothing flammable (unless `force`).
Callers: the `Ignite` dev command, explosions of **Burn** damage (every flammable cell and every pawn
in the radius with `blastIgniteChance`, and the cell it went off in always, so molotovs (F9 left
them for E4) are one projectile def away), a burning pawn (below), a room above 235 degrees, and
the hooks other tasks will use: lightning and short circuits (T3), the fire-starting break (C19).

**Each pass, for every fire:**
1. **Weather.** Outdoors (`Weather::outdoors`), snow `snowBlocksAt` (10%) deep puts it out, and rain
   (`extinguishesFire` and `rainRate`) takes `rainOutPerTick x rainRate` of its size per tick.
   Under a roof nothing happens, so a burning room stays alight in a storm.
2. **Burning.** The fire hurts every flammable thing in its cell: `clamp(0.0125 + 0.0036 x size,
   0.0125, 0.05)` hit points per tick, rounded at random like RimWorld's `GenMath.RoundRandom`
   (the formula is recalled, see `unverified`). A building or item at 0 is destroyed (a normal
   `ThingDestroyed`) and leaves a layer of **ash** filth. A plant takes `plantDamageFactor` (6, ours)
   times that, and goes (`PlantRemoved`, message "burnt") when its hit points are gone.
3. **Size.** With fuel it grows by `growthPerTick` (0.00055) up to `1.75 x min(1, flammability)`;
   without fuel it shrinks by `shrinkPerTick` and is out at 0. A new fire is `startSize` (0.1).
4. **Heat.** It pushes `heatPerSecond x size` RimWorld heat units a second into the enclosed room
   it is in (`TemperatureSystem::addHeat`, the same units as a heater: 21 gives 35 degrees an hour
   in a 25-cell room), to at most `heatMaxC`. Heat pushed outdoors is lost.
5. **Spread.** To every cell within a knight's move (the wiki: "2 straight, or 1 diagonal plus 1
   straight": 20 cells), the chance in a pass is `spreadPerTick x ticks x size x the target's
   flammability`; the 12 cells two away ("embers") need a fire of `emberMinSize` (0.7) and are
   `emberFactor` (0.2) as likely. Walls do not stop embers, as in RimWorld; a stone wall just
   can't burn. Targets are collected and lit after all fires have been looked at, so the order
   fires are visited in changes nothing.
6. **Out.** A fire at size 0 is removed (`FireOut`, message "burnt out"). If it had burned two
   passes the cell is marked **burnt**; one that burned `floorBurnPasses` (10) on a flammable
   floor takes the floor with it.

**A room that gets too hot.** If a fire's room is at or above `autoIgniteC` (235, the wiki's
number), every flammable cell in that room catches with `hotRoomIgniteChance` each pass, and a pawn
in it with `hotPawnIgniteChance`. This is how an indoor fire takes the whole room, as the wiki says.

## Burnt ground and ash

`Fire::burnt(cell)` marks ground that burned (a fire of two passes or more, a plant or thing burnt
up there). It fades after `burntFadeDays` (6, ours). The sim does nothing else with it yet: U14
draws it, and later tasks can slow regrowth. Ash is the existing `Ash` filth (C17), cleaned by the
Cleaning work type.

## Pawns on fire

A pawn in a burning cell catches fire with `catchPerTick x ticks x size x its flammability` per
pass. Flammability is the def's `stats.flammability` or `pawnFlammability` (0.7), times the genes'
`flammability` factor (`Sim` installs it with `setPawnFactor`: Emberkin's FireResistant gene is 0,
so they never burn). `Burning {until}` is a component on the pawn; it lasts `pawnBurnTicks` (1,800)
times 0.75 to 1.25.

While it burns, every pass the pawn takes `pawnDamagePerTick` (2 heat damage per RimWorld second,
the wiki's number) as a hit of Burn damage on a random outside part, **through the heat armor** of what covers it (the same armor rule as bullets: heat armor less nothing is the share of hits changed, half of those bounce off, the rest do half damage, and the piece loses 1 hit point per 4 damage that reached it; `CombatSystem::hurtPawn`), **its job is dropped** (`PawnCaughtFire`
makes the sim `interrupt` it), and the cell under it keeps a fire going (a burning pawn sets the
ground alight, as the wiki says). A downed pawn burns until it is put out or it burns out: it can
die of it. Beating out the fire under a pawn puts it out too (`PawnFireOut`).

**Panic.** A burning pawn that isn't downed, drafted or player-controlled does not think: each time it has
stopped (`Fire::panicStep`, called by the job system instead of the think list) it takes one step to a random
walkable neighbour, preferring one that isn't alight, and its job ends ("it is on fire"). A colonist who is not
alight and within 1.5 m beats the flames out as it runs past. `panic` in `FireRules` turns it off.

**Water.** A burning pawn that has water within `waterRunRadius` (12 cells, ours) runs for the nearest water cell (a greedy step each time
it stops) and is put out when it is next to it. Not here yet: shallow water that can be waded, and the Heat armor of apparel
(devilstrand): `applyDamage` takes the hit as it is.

## Firefighting

**The home area** (`Fire::home`) is the set of cells where colonists with the Firefighter work type
put fires out. It grows by itself: at the first step it is a circle of `autoHomeRadius` (5) round
every building and `startHomeRadius` (12) round every colonist, then a circle of 5 round each new
building (`ThingSpawned` of a finished building). The first `SetHomeArea` command (add or take off
a rectangle) turns the automatic growth off, like RimWorld's "expand automatically". The snapshot
has `homeArea`; saves keep it.

**The job.** `ExtinguishFiresGiver` (task `ExtinguishFires`, the Firefighter work type) offers every
fire in the home area, and every fire under a burning colonist anywhere. The job `ExtinguishFire`
is two steps, `GotoTouch` (to a cell beside the fire, like mining) and `ExtinguishWork`: each game
second it beats `beatPerTick` (0.00167, ours) of size off, converted to seconds, times the pawn's
Manipulation. **Nothing is reserved**, so several colonists fight one fire together, which is how
RimWorld does it. A fire already out when the colonist arrives ends the job as done, not failed.
At size 0 the fire is out (`FireOut`, "beaten"), and so is any pawn burning there. A drafted
colonist is not put on this job (drafted colonists skip the think list).

**Firefoam popper** (`FirefoamPopper` in `Things/Defense.json`, research `Firefoam`, steel 30; numbers unverified).
While any fire burns, each game second a popper looks for a fire within `firefoam.triggerRadius` (3 m, ours). If one
is there it puts out every fire within `firefoam.radius` (9 m) (`FireOut` "foamed") and every pawn burning there,
emits `FirefoamPopped` and is destroyed (single use). A popper that is destroyed by damage bursts the same way
whether or not a fire is near (`Fire::onDestroy`, from the combat before-destroy hook), message "destroyed".

**Firefoam on the ground.** A burst lays a layer of `FireFoam` filth on every cell in its radius. A cell with foam on it counts
as wet: it cannot be lit (`ignite` fails, spread and hot-room ignition skip it), so embers cannot relight what was foamed.
Each hour a layer fades with chance 1 / `foamFadeHours` (12, ours); colonists can also clean it (Cleaning work type).

**Foam turret** (`FoamTurret`, research `Firefoam`, needs power; `firefoamTurret {range 20, radius 2.5, cooldown 10}`, ours). Every
`cooldown` game seconds (staggered by entity id) a running turret shoots the nearest fire in range: fires and burning pawns within
`radius` of it are put out and the ground there is foamed (event `FirefoamPopped`, message "turret"). It is not used up and needs
no line of sight.

A fire that grows faster than the colonists beat it (a big grass fire, a burning house) needs the
home area, hands, and a good start, or a popper.

## What Unreal gets

Events: `FireStarted` (cell, size), `FireOut` (cell, message "burnt out" or "beaten"),
`PawnCaughtFire`, `PawnFireOut`, `FirefoamPopped`, `PawnAction::Extinguish` (a colonist started beating a fire),
`PlantRemoved` ("burnt") and `ThingDestroyed` for what burnt up. Snapshot: `fires` (cell, size,
rewritten when `fireChanges` goes up, which is every pass while any burn), `burnt`, `homeArea`,
`ThingView::onFire`. Commands: `Ignite` (a cell, or a pawn) and `SetHomeArea`. U14 draws fire,
smoke and burnt ground from these ([fire-draw.md](fire-draw.md)); the home area overlay is a U19 job.

## Numbers

All in `FireRules`. Marked `unverified` (recalled from RimWorld, not on the wiki): `maxSize`,
`growthPerTick`, the damage formula, `pawnFlammability`, `pawnDamagePerTick`, `pawnBurnTicks`. The
wiki's own: 235 degrees, 2 damage a second, spread range. Everything else is ours and tunable;
the ones to watch in a playtest are `spreadPerTick`, `plantDamageFactor` and `beatPerTick`
(how big a grass fire gets before two colonists beat it).

## Saves

Section `fire` 1: the counters, the fires, the burnt cells, the home area (cell indices), the
automatic-growth flag, and per burning pawn its `Burning` part. Older saves load with no fires and
a home area made at the first step. `Burning` and the layer are hashed (`Sim::stateHash`).
Commands `Ignite` and `SetHomeArea` are saved by name.
