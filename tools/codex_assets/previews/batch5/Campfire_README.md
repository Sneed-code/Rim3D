# Campfire

Campfire is a one-cell cooking hearth for Rim3D. The stone ring holds an ash bed, charred fuel, and emissive coals. Two forked branches carry a crossbar and a blackened iron cooking pot.

| Specification | Value |
| --- | --- |
| Footprint | 1 × 1 m gameplay cell |
| Mesh dimensions (Blender X × Y × Z) | 0.922 × 0.938 × 0.887 m |
| Hearth height before the spit | 0.270 m |
| Triangles | 3,986 |
| Materials | 1 |
| UVs | One non-overlapping UV0 atlas |

## Files

| File | Contents |
| --- | --- |
| `Campfire.glb` | Game mesh, `Flame` marker, and four embedded textures |
| `Campfire.blend` | Editable Blender 5.3 Beta scene with packed maps |
| `Campfire_BaseColor.png` | sRGB surface color |
| `Campfire_Normal.png` | OpenGL tangent-space normal, +Y green channel |
| `Campfire_ORM.png` | Linear packed map, R ambient occlusion, G roughness, B metallic |
| `Campfire_Emissive.png` | sRGB emissive color for cracks in the coals |
| `preview_front.png`, `preview_side.png`, `preview_34.png` | Front, side, and elevated three-quarter views |
| `preview_close.png`, `preview_scale.png` | Hearth detail and scale with a 1.800 m grey reference figure |
| `contact_sheet.jpg` | Five preview views in a 2400 px-wide, two-row studio sheet |

All textures are original procedural RGB PNGs at 2048 × 2048. Base Color contains surface pigmentation and deposits such as soot. Ambient occlusion lives in the ORM map. The GLB shares the packed texture between the material’s occlusion and metallic-roughness slots and uses `KHR_materials_emissive_strength` with a strength of 3.1.

## Origin and axes

- `Body` is the static mesh. Its origin is the cell center at ground level, `(0, 0, 0)` m in Blender. The spit, branches, pot, fuel, and hearth all belong to `Body`.
- `Flame` is an empty at `(0, 0, 0.288)` m in Blender, centered just above the fuel. In the GLB it is at `(0, 0.288, 0)` m.
- Blender uses metres, +Z up, and −Y toward the working side. The default glTF conversion produces +Y up and +Z toward the working side. Both nodes use unit scale.

## Rebuild and verification

The Blender scripts use bundled NumPy for geometry and texture generation. The separate contact sheet step uses Pillow. These commands rebuild everything from this folder with Blender 5.x on `PATH`.

```powershell
blender -b --factory-startup --python-exit-code 1 -P build_campfire.py
blender -b --factory-startup --python-exit-code 1 -P preview_campfire.py
python -B preview_campfire.py --sheet
```

`build_campfire.py` uses `campfire_textures.py` to generate the atlases and exports with Blender’s default glTF settings. `preview_campfire.py` imports the GLB into a fresh Blender scene and renders five 1600 × 1200 Cycles previews. The GLB check covered triangle count, mesh winding and edge pairing, UV0 overlap, embedded map dimensions and pixel parity, and the marker transform.

The first render review exposed weak bark breakup, an obscured ember bed, and a visible surface transition on the rock crowns. The final pass added fractured bark and soot, opened the front of the hearth, refined the emissive fissures, and matched the stone crown textures to the flanks. The pot bail was lowered onto the crossbar and the scale figure’s ankles were joined before re-rendering.

The asset is complete within this brief. Runtime flame VFX and Unreal import verification remain game integration work.
