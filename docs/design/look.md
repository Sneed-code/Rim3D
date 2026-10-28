# The look: light, camera and the look bench

Task L1 of the look pass ([art-stage.md](../plans/art-stage.md)). Code: `game/Source/Rim3D/SkySubsystem.*` (light, exposure,
fog, grade, `Look.Quality`), numbers in `data/look.json`, the bench in `SmokeTestLook.cpp` and `tools/look_sheet.ps1`.
Unreal side only: the sim never reads any of it. The sky's moving parts (sun path, moon phase, clouds, rain, snow) are
unchanged and described in [sky.md](sky.md).

## Light in real-world units

| Light | Value (`data/look.json`) | Notes |
| --- | --- | --- |
| Sun | 110,000 lux at a clear noon | fades out between 2 degrees below and 3 above the horizon; the sky atmosphere already reddens and dims it near the horizon. Clouds take away the weather's `skyGlow` squared (a storm lets through about a tenth) |
| Moon | 0.5 lux full, a quarter of that at new moon | a bright real moon. It also lights the sky atmosphere (its second sun), so the night sky is a faint moonlit blue |
| Sky light | 1.0 day and night, 0.8 under full cloud | the real-time capture of the atmosphere is already in real units, so it needs no fudging |
| Lightning | 30,000 lux at its peak | about three times a storm's daylight |
| Lamps, fires, muzzle flashes | candelas, as before (`FireSubsystem`, `FightSubsystem`) | at night they are the brightest things on screen, at noon they hardly show, as in life |

The sun casts virtual shadow maps out to 300 m, contact shadows (4% of the screen) for the small gaps, shadows from the
volumetric clouds onto the ground (fading out as the sky closes over, since the weather already dims the sun then), and lights
the volumetric fog.

## Exposure

The camera is held near a target exposure (EV100) worked out from the light on open ground: the sun's share that falls
on it, the sky's own light (16% of the clear sun, fading through twilight) and the moon. A clear midsummer noon is EV 15;
for every stop the light drops, the target drops 0.8 of a stop. So a night (about 18 stops darker than noon) is held about
3.5 stops darker on screen than a day, a dusk a little under one: day, dusk and night each read as what they are.

The histogram auto exposure may move 0.75 of a stop brighter than the target (into a dark room) and 0.25 darker (bright snow),
easing at 2 stops a second up and 1.5 down. When the target moves more than 2 stops in one frame (a time jump, a load,
`Sky.Hour` in a test) the camera is cut so it jumps there instead of easing over seconds. `Look.Exposure <EV>` holds a fixed
exposure for tuning.

Unlit materials would turn black at EV 15, so the blueprint and build ghost material (`M_BuildingGhost`) multiplies its colour by
the inverse of the exposure: after pulling L1, delete `game/Content/Rim3D/Generated/Buildings/M_BuildingGhost.uasset` (the
commandlet will not overwrite it) and rerun the `BuildingMaterials` commandlet (`assets/manifest.json`).

## Fog and haze

The height fog's colour is now the sky atmosphere's own (`r.SupportSkyAtmosphereAffectsHeightFog`), so it is blue in the
distance at noon, red at sunset and dark at night: no more purple. As the sky clouds over, fogs up or snows, a grey (or
snow-white) veil lit by the sky's light is mixed in. The density still follows the weather's sight factor (sky.md), plus a low
mist (0.03) while the sun is between 3 degrees below and 14 above the horizon and it is not raining. Volumetric fog (High and
Epic) out to 90 m makes the light shafts through the trees at dawn and dusk.

## Film and grade

The engine's filmic tone curve at its neutral numbers (slope 0.88, toe 0.55, shoulder 0.26, white clip 0.04), soft bloom
(0.35), a light vignette (0.2), no lens flares or grain, screen-space ambient occlusion where Lumen is off, local exposure at
0.85 so bright skies and dark shade both keep detail. One grade per season, blended between the seasons' middle days:

| Season | White balance | Saturation | Contrast | Gain |
| --- | --- | --- | --- | --- |
| Spring | 6500 K (neutral) | 1.04 | 1.02 | a touch warm |
| Summer | 6700 K (warmer) | 1.07 | 1.05 | warm |
| Autumn | 6900 K (warmer) | 1.00 | 1.04 | orange |
| Winter | 6350 K (a little colder) | 0.90 | 1.00 | blue |

## Look.Quality

| Step | Engine scalability | Ours |
| --- | --- | --- |
| 0 Low | 0 (no Lumen, simple shadows) | no volumetric fog, no contact shadows, no volumetric clouds |
| 1 Medium | 1 (no Lumen) | contact shadows and clouds |
| 2 High (default) | 2: Lumen global illumination and reflections, virtual shadow maps | volumetric fog (12 px grid) |
| 3 Epic | 3 | volumetric fog at 8 px |

The render resolution is left as the player set it. Set it in the console (`Look.Quality 1`) or for good in
`Saved/Config/Windows/Engine.ini` under `[ConsoleVariables]`. A settings screen comes with V24.

## On screen

`Sim.ShowTimings` is off by default (it was on). The key list in the bottom right corner of the overseer and direct-mode HUDs
is folded to a small "F1  keys" box; F1 opens and closes it (`UI.KeyHelp`).

## The look bench (smoke test `look`)

Every look task ends with it. Seed 42, a 256 m Crashlanded map; a 7 x 6 room with a bed, a table, an armchair and two lit torches
is built on open ground near the colony's stockpile, and the clock run to 10:00. The game stays paused for every shot so the
pawns stand in the same places each run; MetaHumans on. The shots (Full HD, no UI except `hud`):

1. `overseer_high`: the overseer zoomed out over the colony, clear midsummer noon.
2. `overseer_low`: low across the field toward the room, 10:30.
3. `colonist_closeup`: the first colonist's head and shoulders from in front, 10:00.
4. `room_lit`: inside the room at 21:00, roof on, lit by its torches.
5. `dusk`: the low sun in the west over the field, 17:48.
6. `rainy_night`: an hour of rain, the overseer over the room at 23:00.
7. `hud`: the overseer view at noon with the HUD as a new player sees it.
8. `snowy_day`: a new game on the same seed snows hard through the night; the overseer at 11:00 on a midwinter morning.

`tools/smoke.ps1 -Test look -LookLabel <word>` runs it, then `tools/look_sheet.ps1` copies the shots into
`game/Saved/Look/<date>_<time>_<word>/` and makes `sheet.png` there: each shot with the last run's on the left. Compare any two
runs with `look_sheet.ps1 -LookDir game\Saved\Look -Run <folder> -Against <folder>`. The notes in `game/Saved/Smoke/look.json`
give each shot's sun, moon, fog and exposure numbers.

## Cost

The `perf` test (1 km colony, raid, rain, 1440p) in the editor at High: GPU 3.2 to 5.5 ms a frame, against 3.4 to 6.6 ms in
G3's packaged run before L1 (engine default quality then), so High with Lumen, volumetric fog and contact shadows costs no more
GPU than before. Frame rates in the editor (57 to 74 fps, a 13 to 15 ms editor game thread) match G3's editor numbers; a packaged
run belongs to L10. The look bench draws at 134 to 168 fps at Full HD.

The bench's known flaws, all placeholders the next tasks replace: the plant cones (`VertexColorMaterial`) show brightly at night,
and the first colonist wears a black placeholder hat.

## Not done

Stars; a moon disc; emissive flames that glow by day (V15's Niagara fire); per-room exposure in direct mode beyond the 0.75 stop
window; depth of field (V12). Wind in grass and trees is L2 and L6.
