# Wetness: rain makes things wet

sneed (2026-11-20): "buildings get wet, have ray tracing light etc." Before this, only the ground soaked in rain
([sky.md](sky.md), "Wet ground"); walls, roofs, furniture, trees, rocks and people stayed dry, and the floor of a roofed
room got as wet as the street. All of it is cosmetic: the sim's weather (rain, its `wetness`) decides, Unreal shows.

Code: `game/Source/Rim3D/WetnessSubsystem.*` (the game side), `WetnessShared.h` (names both sides share),
`game/Source/Rim3DEditor/WetLookCommandlet.*` (the assets and the material changes).

## What gets wet

| What | How wet | Soaks / dries |
|---|---|---|
| Surfaces: walls, roofs, doors, furniture, Codex and Fab buildings, trees, plants, rocks, guns, animals, vehicles | one world wetness: 0.5 plus the weather's `wetness`, capped at 1, while it rains | through in about 10 game minutes; dry about 2 game hours after the rain |
| The ground | as before (USkySubsystem), now kept dry under roofs | about 30 minutes / 4 hours (unchanged) |
| People (skin, hair, clothes) | each pawn its own: the same target while outdoors in rain, 1 at once when swimming in deep water | about 5 game minutes / 40 game minutes under a roof or once the rain stops |

How a wet surface looks (the material code in the commandlet):
- **Darker where porous:** water fills pores and fibres, so the base colour goes down to x0.55 scaled by how rough
  the surface is (roughness x1.4, capped at 1): wood, plaster, stone, bark and cloth darken a lot, metal and glossy paint
  hardly at all.
- **Glossy:** roughness falls toward 0.12 (a film of water) whatever was under it; ray-traced reflections then show the
  sky and lights in it.
- **Facing:** faces pointing up are wettest, upright faces 0.6 of that, faces pointing down stay nearly dry (the
  underside of a table outside, eaves).
- **Cover:** nothing under a roof or an upper floor gets wet. `T_WeatherCover` holds, per map cell, the height of the
  highest roof or floor over it (metres over the world origin; the mountain's roof over mined-out rock counts, rock
  itself is open on top). A pixel lower than that (with 15 cm to spare) is dry. The cell is looked up half a cell out
  along the surface's normal, so a wall's outside face counts the open cell outside it (wet) and its inside face the
  room (dry). The ground's own `Wetness` is masked the same way, so a roofed room's floor stays dry.

People: `UWetnessSubsystem` writes each pawn's wetness into custom primitive data 12 on every mesh of the pawn. The
MakeHuman skin, hair and jewellery already read it (`MhAddWetness`, the creator's Wet slider); clothes read it after
`-run=WetLook` (worn mode). A pawn counts as outdoors when the middle of the body (1 m over the feet) is not under cover.

## Assets (not in Git)

`-run=WetLook` makes, in `/Game/Rim3D/Generated/Weather`:
- `MPC_Weather`: `Wetness` (written by the game every time it changes) and `CoverMap` (the world origin in cm in xyz, one
  over the texture size in w).
- `T_WeatherCover`: 1024 x 1024 single floats, nearest, no mips, never streamed, so the game rewrites it with
  `UpdateTextureRegions` when a roof, floor or the mountain changes (`Snapshot::roofChanges`, `roofCollapses`,
  `Map::upperFloorChanges`, `Map::rockChanges`). A map wider than 1024 m is covered as far as the texture reaches.

Then it changes every opaque or masked, lit, surface material under `/Game/Rim3D/Generated` (except Blood, Fights, Sky,
MakeHuman, Animations, Water and Weather): whatever fed Base Colour and Roughness goes through the wet look first (nodes
tagged `WetLook...`, so a material is changed once; a wrap from an older run, or in the wrong mode, is taken out and
done again). Clothes and people's own materials (`M_Pawn...`: skin, hair, eyes, cloth) take worn mode; a material with its own `Wetness` scalar
parameter (the terrain) gets that parameter masked by the cover; one whose `Wetness` comes from custom primitive data (the
MakeHuman bodies) is left alone. Material instances follow their parents. Run it again after any commandlet that remakes
materials (TerrainMaterials, SurfaceMaterials, PlaceholderArt, FoliageMaterials, TreeArt, GunArt, AnimalArt, ClothesArt,
ShuttleArt, ShieldArt); `-List` shows what it would change.

It also makes `M_WetSolid`, our own copy of the engine's VertexColorMaterial (vertex colours, lit, roughness 0.5) with
the wet look and the damage look on it. Buildings, upper floors and wreck pieces whose stuff has no surface material
(plain colour walls) are drawn with it (`WetnessShared::LoadSolidMaterial`, the engine's own before the commandlet runs).

The cover texture is sent again whenever its GPU copy is made again (an editor build can finish building the texture
after the game has started), and the cover's place (`CoverMap`) again whenever the world origin moves.

```
UnrealEditor-Cmd.exe game\Rim3D.uproject -run=WetLook -unattended -nosplash -nullrhi
```

Without the assets, only people get wet (their materials already have the wet look).

## Console

`Wet.Enabled` (1; 0 keeps everything dry), `Wet.Override` (-1 follows the weather; 0 to 1 holds surfaces, and people
outdoors, at that wetness, for screenshots and checks). `Wet.Dump` logs the state: the cover texture's format and
whether it is usable, the cells under a roof, how often the cover was made and sent, the surface wetness shown.

## Tests

Smoke test `sky_rain` (three game hours of rain at the foot of the mountain) notes and checks the surfaces' wetness
(at least 0.3), the wettest pawn, the covered cells and that the cover was written and reached its texture. Pictures for sneed: the look bench's
rainy night, and `Wet.Override 1` against `Wet.Override 0` on a house with a roof.

## Later

Drips from eaves and puddles that grow in hollows; rain streaks running down upright faces; wet clothes clinging;
snow lying on roofs and furniture (snow on the ground is USnowSubsystem's); people drying faster by a fire.
