# WeaponPart_LongBarrel

Original fictional Rim3D ground prop. The barrel runs along +X toward the muzzle and rests at Z = 0 in Blender. The origin of `Body` is (0, 0, 0) at the bottom centre of the bounds. It is the only mesh and has no moving pivots. The GLB uses standard glTF +Y up and metre units.

| Property | Value |
| --- | --- |
| Blender bounds (X × Y × Z) | 0.60000 × 0.05921 × 0.06130 m |
| Triangles | 2,944 |
| Materials | 1 |
| UVs | UV0, unique islands with padded borders |

## Textures

All three procedural 1024 × 1024 PNGs are delivered separately and embedded in the GLB.

- `WeaponPart_LongBarrel_BaseColor.png` uses sRGB and contains surface colour.
- `WeaponPart_LongBarrel_Normal.png` uses OpenGL tangent space (+Y).
- `WeaponPart_LongBarrel_ORM.png` uses linear data with red for occlusion, green for roughness and blue for metallic.

## Review and rebuild

All five 1600 × 1200 PNGs were rendered in Cycles after importing the exported GLB into an empty Blender scene. `preview_scale.png` uses a 1.80 m human reference. `contact_sheet.jpg` combines the five views.

After the first render, the steel finish was toned down, the chamber-end contours were refined, the cloth folds and knot were reworked, and the studio framing was adjusted.

Run `python build_asset.py` with NumPy, Pillow and Blender 5.x. Use `--blender` to select another Blender executable.

The muzzle opening ends in a shallow blind recess. The rings and chamber-end fittings are cosmetic. The asset is delivered for Unreal import. Live Unreal import was outside this delivery.
