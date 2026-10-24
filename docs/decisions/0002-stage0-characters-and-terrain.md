# 0002: Stage 0 character and terrain tests

Date: 2026-10-24. Answers the MetaHuman and runtime terrain risks in `architecture.md`.

## How it was run

`game/Source/Rim3D/Stage0Test.cpp` (since renamed `SmokeTest.cpp`, see `docs/design/smoke-tests.md`), started on sneed's PC (RTX 5080, Ryzen 7 9800X3D) with:

```
UnrealEditor.exe C:\Dev\Rim3D\game\Rim3D.uproject /Game/Levels/DefaultLevel -game -fullscreen
  -ResX=2560 -ResY=1440 "-ExecCmds=r.SetRes 2560x1440f" -Stage0Test=crowd     (or =terrain)
```

It places its own camera, measures 10 to 15 seconds per phase, saves screenshots and
`game/Saved/Stage0/<test>.json`, then quits. Characters are the Game Animation Sample's
`SandboxCharacter_CMC` (motion matching, CharacterMovement), driven in circles; the MetaHuman
phases hide that body and put the sample's MetaHuman (Kellan, with groom hair) on top using its
pose. 1440p native with TSR, no DLSS (not installed yet). It runs in the editor binary, which is
slower on the CPU than a shipped game, so these are worst-case numbers.

## Results

| Phase | fps | Game thread | GPU |
| --- | --- | --- | --- |
| 10 MetaHumans, close up | 48 | 4 ms | 4.5 ms |
| 100 MetaHumans running, overseer view | 28 | 35 ms | 4.3 ms |
| 100 mannequins running | 49 | 19 ms | 3.3 ms |
| 200 mannequins running | 20 | 46 ms | 3.5 ms |
| 1 km terrain, overseer view | 56 | 3.5 ms | 3.3 ms |
| 1 km terrain, low view to the horizon | 55 | 3.2 ms | 4.2 ms |
| 1 km terrain, whole map | 58 | 2.6 ms | 3.8 ms |

Terrain rows sit at the 60 fps cap. The terrain mesh (1,024 chunks of 32 m, 1.1 million
vertices) built from the sim's height map in 155 ms.

## Findings

1. **Rendering is not the problem; animation on the CPU is.** 100 MetaHumans with hair cost the
   graphics card about 4 ms of its 16 ms budget. The game thread spends about 0.2 ms per
   mannequin and 0.35 ms per MetaHuman each frame on motion matching, CharacterMovement and pose
   copying.
2. **Simple stretching doesn't make body types.** Scaling the MetaHuman wider or thinner pushes
   body through the clothes, and the sample's skin material has no colour setting to tint, so
   the Impid's red skin didn't work. The horns were too small to read at that distance.
   Seeded variety needs real body shapes (MetaHuman body types or blend shapes) and our own skin
   material with a tint setting.
3. **Runtime terrain is cheap.** A 1 km sim-driven mesh builds in a fraction of a second and
   draws in about 4 ms. How good it looks next to Unreal's Landscape still needs Megascans
   materials (downloaded from Fab) and sneed's eye.

## Decision

- **Keep MetaHumans for colonists**, within these limits:
  - The sim moves pawns, so pawns drop CharacterMovement and only animate.
  - Full motion matching only for about the 20 pawns nearest the camera. Others use cheaper
    animation and update less often with distance (Unreal's animation budget allocator and
    update rate settings). Far pawns use lower-detail bodies and no strand hair.
  - Re-measure in a shipped build in Stage 1. The target stays at 100 animated pawns at 60 fps.
- **Body variety**: build seeded looks from MetaHuman body types and morph targets plus our own
  skin material with tint and xenotype settings. Horns and other xenotype parts are attached
  meshes sized for the overseer view. This is a Stage 1 art task.
- **Terrain: runtime mesh from the sim**, chunked at 32 m. The material pass with Megascans and
  a side-by-side with Landscape happens once sneed has downloaded the packs.
