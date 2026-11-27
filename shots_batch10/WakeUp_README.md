# WakeUp

WakeUp is a six-pocket stimulant blister strip for Rim3D. Four yellow capsules remain sealed, two pockets have been pressed through the foil, and one capsule lies beside the strip. The underside carries a generic printed grid and broken yellow stripe, with no lettering or logos.

## Mesh

| Property | Value |
| --- | --- |
| Asset | [WakeUp.glb](WakeUp.glb) |
| Dimensions in Blender X × Y × Z | 0.105509 × 0.048000 × 0.011418 m, including the loose capsule |
| Triangle count | 1,490 |
| Mesh and pivot | `Body`, at the bottom centre of the full footprint |
| Transforms | Unit scale, zero rotation and translation |
| UVs | One non-overlapping UV0 atlas, no lightmap UV |
| Materials | `WakeUp_Surface` for foil and capsules, `WakeUp_Clear_PET` for the transmissive blisters |

All components are part of `Body`. Blender uses metres, +Z up and +X along the strip. The front view looks in from Blender −Y. The GLB uses the default exporter conversion to +Y up, with +X along the strip and the front facing +Z.

## Textures

Both materials share the same three procedurally authored 1024 × 1024 atlases. The GLB embeds all three maps.

| File | Encoding and channels |
| --- | --- |
| [WakeUp_BaseColor.png](WakeUp_BaseColor.png) | sRGB pigment and material colour without baked lighting, shadows or AO |
| [WakeUp_Normal.png](WakeUp_Normal.png) | Linear tangent-space OpenGL (+Y) normal map for the crimp, capsule seam and fine surface wear |
| [WakeUp_ORM.png](WakeUp_ORM.png) | Linear packed map with ambient occlusion in R, roughness in G and metallic in B |

## Previews and review

[Contact sheet](contact_sheet.jpg) · [Front](preview_front.png) · [Side](preview_side.png) · [Three-quarter](preview_34.png) · [Detail](preview_close.png) · [1.8 m scale](preview_scale.png)

The five studio previews were rendered at 1600 × 1200 in Cycles on an RTX 5080, after re-importing the exported GLB into a fresh Blender scene. The contact sheet is a 2400 × 2808 JPEG at quality 85 and includes an underside view in its sixth cell.

The first render pass showed washed-out foil, pale capsule halves and pressed pockets that reflected like intact blisters. The revised asset has more defined silver crimping and light scuffs, a clearer capsule seam, asymmetric crushed plastic, and longer folded foil leaves around each tear. The final side view checks the low profile and where the foil meets the surface. The underside view checks both tears and the printed grid.

## Rebuild and validation

With Blender 5.x and Python with NumPy and Pillow installed, run:

```powershell
python run_wakeup.py all
```

The launcher defaults to the installed Blender 5.3 Beta. Set `--blender` to use another Blender executable. The build scripts generate the maps, model and export the asset, render the re-import, assemble the contact sheet and verify the final files.

The brief deliverables are complete. Blender verified the re-import and appearance. Runtime import in Unreal Engine 5.8 remains unverified. The clear material uses `KHR_materials_transmission` and `KHR_materials_ior` on a single-surface shell.
