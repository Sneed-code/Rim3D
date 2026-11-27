# SleeperShard

SleeperShard is a sealed AI persona core for Rim3D. Stepped gold filaments run through a faceted violet-blue crystal. A graphite cage, tarnished gold-alloy plates, engraved geometric markings, and four short feet protect the core.

## Asset specifications

| Property | Value |
| --- | --- |
| File | `SleeperShard.glb` |
| Dimensions in Blender (X × Y × Z) | **0.16122 × 0.16122 × 0.18000 m** |
| Triangle count | **4,748** |
| Mesh object | `Body` |
| Pivot | `(0, 0, 0)` m at the bottom centre of the footprint, on the plane of the four feet |
| Blender axes | +Z up, front faces −Y, metres, unit object scale |
| glTF axes | +Y up, front faces +Z, metres, Blender exporter defaults |
| Materials | `SleeperShard \| Cage and circuits` and `SleeperShard \| Violet crystal` |
| UVs | One non-overlapping UV0 atlas shared by both materials |

The cage, crystal, and filaments are joined into `Body`. The asset has no moving or swappable parts. The crystal uses glTF transmission, IOR, and volume for tint and refraction.

## Textures

Each 2048 × 2048 PNG is included separately and embedded in the GLB. The maps are procedurally generated.

| File | Interpretation |
| --- | --- |
| `SleeperShard_BaseColor.png` | sRGB material colour, free of baked lighting, shadows, and AO |
| `SleeperShard_Normal.png` | Linear OpenGL tangent-space normal, +Y green. Flip green if importing the PNG manually as a DirectX normal in Unreal. |
| `SleeperShard_ORM.png` | Linear packed AO (R), roughness (G), metallic (B) |
| `SleeperShard_Emissive.png` | sRGB cool crystal glow with subtle warm filament accents |

## Previews and review

The five 1600 × 1200 previews were rendered with Cycles on the RTX 5080 after re-importing the exported GLB into a fresh Blender scene. `preview_scale.png` places the asset on the same ground plane as a 1.800 m grey human silhouette. `contact_sheet.jpg` arranges the five views in a 2 × 3 grid at 2398 × 1344, JPEG quality 85.

The first render pass showed stone-like mottling on the cage and pale reflections that obscured the crystal. The second pass reduced broad noise and concentrated wear at exposed edges, strengthened the engraved patterns, deepened the crystal with tinted transmission, and made the suspended filaments clearer. The base and feet were integrated and completely buried joint triangles were removed. The final re-import confirmed the mesh name, pivot, dimensions, triangle count, UV0, and four embedded maps.

## Runtime status

The GLB carries a steady emissive map. For a slow pulse in Unreal, drive the crystal material's emissive multiplier with `0.40 + 0.10 * sin(2 * pi * Time / 5.5)`, where `Time` is in seconds. The pulse and Unreal-specific refraction setup are runtime material work. Validation was performed in Blender 5.3 Beta.

## Rebuild

With Blender 5.x and Python 3 with NumPy and Pillow, run `python build_sleeper_shard.py` from this folder. Pass `--blender` with the executable path to select a different Blender 5.x installation. The build runs `sleeper_shard_blender.py` headlessly for model construction, default GLB export, and re-imported preview renders. The `--review` option recreates the first pass under `.sleeper_review`.
