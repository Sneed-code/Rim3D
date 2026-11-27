# RawPotatoes

Raw crop food stack for Rim3D. Six whole potatoes, two with reddish skin, sit around one cut half with a pale cream interior. The tuber shapes, larger eyes, and bevel around the cut are modelled. Soil, fine eyes, and skin grain are procedural.

| Asset property | Value |
| --- | --- |
| Deliverable | `RawPotatoes.glb` |
| Bounds in Blender (X × Y × Z) | 0.23954 × 0.18180 × 0.09488 m |
| Geometry | 1,960 triangles across seven closed shells |
| UV0 | Eight non-overlapping charts with padded atlas gutters |
| Material | One opaque PBR material, `RawPotatoes_PBR` |
| Part and pivot | `Body`, static, at the bottom centre of the heap, (0, 0, 0) |
| Axes | Metres, Blender +Z up with the cut facing generally toward −Y. The default Blender glTF export is +Y up. |

## Textures

All three 1024 × 1024 PNGs are embedded in the GLB and supplied separately.

| File | Interpretation |
| --- | --- |
| `RawPotatoes_BaseColor.png` | sRGB surface colour with no baked lighting, shadows, or ambient occlusion. |
| `RawPotatoes_Normal.png` | Linear tangent-space normal, OpenGL +Y. Flip green if importing this standalone map into an Unreal material that expects DirectX normals. |
| `RawPotatoes_ORM.png` | Linear packed map. Red is ambient occlusion, green is roughness, and blue is metallic (zero). |

## Previews and review

`preview_front.png`, `preview_side.png`, `preview_34.png`, `preview_close.png`, and `preview_scale.png` are 1600 × 1200 Cycles renders of the GLB re-imported into a fresh Blender scene. The scale view uses a 1.80 m silhouette. `contact_sheet.jpg` assembles all five views in a 2400 px wide, two-row grid.

The first render showed an overly tall pile with perched potatoes, sharp-looking dents, and an almost featureless cut face. The revision gave the two upper potatoes more grounded contact, reshaped the eye depressions, added clearer soil and lenticel detail, and brought out the subtle vascular ring and rind on the cut face. The final mesh sits at ground level with its asset pivot at the centre of the footprint.

All brief deliverables are complete. The GLB was re-imported and visually checked in Blender 5.3 Beta. Unreal project import was outside this delivery.

## Rebuild

Run from this folder using Python with NumPy and Pillow installed.

```powershell
python build_rawpotatoes.py "C:\path\to\blender.exe"
```

The script generates the PNG textures, models and UVs the heap in headless Blender 5.x, exports the GLB with Blender's default glTF settings, renders it in a separate Blender invocation, and assembles the contact sheet.
