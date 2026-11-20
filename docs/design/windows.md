# Windows in walls

Code: `ColonySim/Windows.h`, `Private/Windows.cpp`; the light in `Lights::lightThroughWindows`, bullets and grenades in
`CombatSystem::trace` and `CombatSystem::breakWindow`, the work in `JobSystem::windowWork` (GatherJobs.cpp) and the
`WindowGiver` (WorkGivers.cpp), the tools in `BuildPlanner` and the `PlaceBlueprintCommand` handler. Defs: `WindowRules`
in `Rules.json`, `building.takesWindow` on `Wall` and `AncientWall`, the tools `WindowTool` and `RemoveWindowTool` in
`Things/BuildTools.json`, the `Window_Shatter` sound cue. Tests: `Tests/WindowTests.cpp`. Drawing:
`game/Source/Rim3D/BuildingSubsystem.cpp` (`BldWindowParts`), glass material from the `BuildingMaterials` commandlet.
Not a plan task (sneed, 2026-11-19).

sneed: windows are a free feature added to any wall to let light in; removable, and destructible. RimWorld has no windows,
so every number here is ours.

## What a window is

A window is **part of a wall**, not a building of its own. Any finished one-cell wall whose def says
`building.takesWindow` (the built `Wall` of every stuff, and the ruins' `AncientWall`) can have one. The wall keeps its hit
points, its stuff, its roof and its place in the room: it still blocks walking, so a room with windows is still a room,
keeps its warmth and its roof. What the window changes is **how tall the wall is for sight, light and cover**: up to the sill
only (`WindowRules.sillDm`, 0.9 m) instead of the whole height. So:

- **Sight** passes over the sill. Pawns inside see out and pawns outside see in (stealth, raiders spotting colonists, turrets).
- **Cover**: a pawn behind a window has the sill's 0.9 m in front of it, like sandbags: crouched it is mostly hidden, standing
  it shows from the waist up.
- **Light**: lamps shine through (the sill is under `LightRules.blockingHeightDm`), and **the sun comes in** (below).
- **Bullets** above the sill fly through the opening. Above the lintel (`lintelDm`, 2.1 m) the wall stops them as before. The
  first bullet through **breaks the glass** (event `WindowBroken`, sound `Window_Shatter`) and flies on; so does a thrown
  grenade or molotov. A **blast** in reach breaks it too (and damages the wall as always). Glass has no hit points: one hit
  breaks it.
- A **broken window** lets sight, light and bullets through just as before; it only shows (no glass is drawn). It can be glazed
  again.

The wall's height lives in its `Footprint::obstacleDm`, which saves already keep; the `Window` component (broken or not) is
saved in its own section, `windows`. Older saves load with no windows. The state hash takes in a window only where there is one,
so old replays keep their hashes.

## Light through a window

Every window with **open sky on one side** and a **roofed cell on the other** lights the roofed side: `lightGlow` (0.8) of the
sky's light right behind it, falling in a straight line to nothing at `lightRadius` (6 m), stopped by tall things like a
lamp's light. The layer keeps only the share; `lightAt` multiplies it by the sky's glow of the moment (time of day, weather,
the aurora's floor), so a room is bright at noon, dim on a stormy day and dark at night with no rebuild. A window between
two roofed rooms lets no sun in. With 0.8 of a clear noon sky the cells near a window are above 0.6, so work and walking are
at full speed there (`LightRules`); the far corner of a big room may still want a lamp.

`lights().windowShareAt(cell)` gives the share. The layer is rebuilt with the lamps (a window cut or filled counts as a
building change: `Map::noteBuildingChange`).

## Cutting, glazing and filling in

Windows cost nothing (sneed). On the architect menu's **Structure** tab are two tools (Things defs of category `BuildTool`, never
things): **Window** and **Fill in window**. Dragged along a wall line, each cell that can take the order is marked (designations
`AddWindow` and `RemoveWindow`); a `PlaceBlueprintCommand` of a tool def does the marking, so the menu and placing code need
nothing new. `BuildPlanner::preview` says which cells can (a finished wall without a whole window; a window to fill in), with
no cost.

A construction worker (task `WindowWork`, after `RemoveFloor`) walks beside the wall and works `addWork` or `removeWork` game
seconds (300 each, ours) at construction speed, like taking up a floor; then the window is cut, glazed or filled in (event
`WindowChanged`, `message` "added", "glazed" or "removed"). The **Window** tool on a broken window glazes it. `CancelDesignations`
takes marks off. A `DesignateCommand` with `AddWindow` or `RemoveWindow` marks an area the same way.

Taking the wall down takes the window with it.

## Drawing

A wall with a window is its linked wall (posts and arms, base course and cap, [building-draw.md](building-draw.md)) cut at the
sill and the lintel, and between them an opening along the wall's line: 10 cm of wall at each cell edge, a stone sill a little
proud of the wall, a dark frame and a pane of glass. Windows side by side read as a row with a jamb between each. At a corner or
a T the post stays whole and only the arms along the line open. The glass is its own mesh in the window glass material
(`M_WindowGlass`: lit, translucent, smooth, two-sided) and **casts no shadow**, so the sun lights the floor inside; with ray
tracing on the light comes in through the opening as it would. A broken window has the frame and no glass. Without the generated
material (a fresh clone before the commandlet runs) windows are drawn open, with a warning in the log.

**Daylight inside** (`WindowLightSubsystem`): the nearest `Windows.MaxLights` (24) windows that let the sky in get a rect light
the size of the opening, just inside the wall and facing into the room, at `Windows.LightCd` (15000 cd, bright sky and sunlit ground seen through the opening; 6000 was still dim on the PC, 2026-11-20)
times the sim's sky glow of the moment, so it follows the day, the weather and the night. The nearest `Windows.ShadowLights`
(8) cast shadows. The side it lights is the side the sim lights (`windowShareAt`). And with the camera **under a roof** the eye
opens up further: the auto exposure may go `exposure.indoorBrighterEV` (5 stops, a real indoor photo is 5 to 6 stops under daylight; 3 was still dim, `data/look.json`) below the outdoor target
instead of `brighterEV`, easing in over a moment, so a room lit through its windows reads as lit instead of black next to the
bright outside (sneed, 2026-11-19: the first noon picture was far too dark).

The look bench (`look` smoke test) cuts two windows in its room's north wall and one in the second cell of each stuff of the
sample wall (granite's broken), and takes an extra shot, `windows`, from inside the room at noon; it fails if the two windows there have no daylight lights. A second shot, `sunbeams`, looks down at the floor under those windows at
half past eight on a midsummer morning, when the sun (azimuth about 128 degrees, 41 degrees up) shines in through them. **Sun
patches are the real sun**: the opening and its frame cast the sun's shadow and the glass casts none, so wherever the sun faces a
window a bright patch falls on the floor and furniture, and with ray tracing it bounces round the room. At noon the sun is in the
south (world -X) and high, so a window in a wall along X, like the room's north wall, gets no patch then (the first two noon
pictures, 2026-11-19 and 20).

**Dust in the air** (sneed 2026-11-20: photoreal and subtle): while the camera is under a roof, each closed, roofed room with a
window near the camera (the 8 nearest, `Windows.DustRooms`) gets a local fog volume: the ellipsoid inside its bounds from floor
to roof, with an even haze (`Windows.Dust`, 0.3 extinction per metre, warm grey albedo) that the volumetric fog lights where the
sun comes in through a window, so the beam shows as a soft shaft across the shadowed room while the outdoors stays clear.
Under a roof the volumetric fog also starts at the camera (outdoors it starts 5 m off) and reaches 30 m (`data/look.json`
`indoorDust`). The volume's size at scale 1 is `Windows.DustBaseCm` (50 cm radius; the `sunbeams_base500` test shot tries 500). The `sunbeams_fogtest` shot holds thick fog in the room (`Look.IndoorFogDensity`) to show the volumetric fog is lit there. Tries that did not work: thickening the fog everywhere four times (faint shafts, milky view outside), and a
box with a volume material (the volumetric fog takes volume materials from particles only, so nothing showed). Needs a quality
step with volumetric fog (High, Epic). The `sunbeams` shot notes the fog's numbers and the dusty rooms.

## Not yet

- Shards on the ground and a pane cracking before it breaks; curtains and shutters (sneed: not needed).
- A broken window letting the room's warmth out (temperature treats the room as closed).
- Windows on upper levels light only their own sight and cover; the sun through them waits for the light layer to go upstairs.
- Mood: there is no "dark room" thought yet ([light.md](light.md) "For later tasks"); when there is, windows count by themselves.
