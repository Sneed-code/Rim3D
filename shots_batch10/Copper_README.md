# Copper

Rim3D metal resource for Unreal Engine 5.8. Three tapered, rounded cast copper ingots and one loose coil of thick copper wire share a single material. Cast pits, brown oxidation, blue-green verdigris, and freshly scraped copper are authored in the surface maps.

| Property | Value |
| --- | --- |
| Game model | [Copper.glb](Copper.glb) |
| Dimensions in Blender (X × Y × Z) | 0.248787 × 0.145220 × 0.071556 m |
| Triangle count | 1,460 |
| Materials | 1, Copper &#124; Cast and Drawn |
| UV0 | Non-overlapping atlas with padded charts |
| Part and pivot | `Body` is the only mesh object. Its origin is at the bottom centre of the horizontal bounds, with unit scale and no moving parts. |
| Axis and units | Blender uses metres and +Z up. GLB uses the Blender glTF exporter's default +Y up and retains metre scale. |

## Textures

All three 1024 × 1024 PNGs are authored procedurally and embedded in the GLB.

| Texture | Encoding and content |
| --- | --- |
| [Copper_BaseColor.png](Copper_BaseColor.png) | sRGB, surface colour with no baked lighting, shadows, or AO |
| [Copper_Normal.png](Copper_Normal.png) | Linear, tangent-space OpenGL normal map (+Y) |
| [Copper_ORM.png](Copper_ORM.png) | Linear, red ambient occlusion, green roughness, blue metallic |

## Previews

[Contact sheet](contact_sheet.jpg) · [Front](preview_front.png) · [Side](preview_side.png) · [Three-quarter](preview_34.png) · [Close-up](preview_close.png) · [Scale](preview_scale.png)

The five 1600 × 1200 previews were rendered in Cycles with OptiX from a fresh import of the exported GLB. The scale preview places Copper beside a plain 1.800 m silhouette. The studio and silhouette are preview scene objects.

After the first full render, the broad highlights were reduced to restore contrast in the copper and oxidation. The ingots were spaced to remove penetration and the coil was reshaped to sit closer to the ground with raised wire ends. The cast relief was refined, the texture transition at the bevel was blended, and the close-up was reframed to show both the cast and drawn surfaces.

## Rebuild

From this folder on Windows with Blender 5.x and Python with NumPy and Pillow installed, run:

```powershell
python Copper_textures.py textures
python Copper_run.py all
python Copper_textures.py sheet
```

`Copper_build.py` builds and exports the mesh, then re-imports it for rendering. `Copper_layout.py` defines the atlas layout shared by the Blender and texture scripts. The launcher prefers the installed Blender 5.3 Beta and uses Blender 5.2 as a fallback.

## Completion

All brief deliverables are complete. The GLB was checked for triangle count, mesh winding, UV overlap, embedded texture contents, and Blender re-import.
