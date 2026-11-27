# WeaponPart_ExtendedMagazine

Original Rim3D ground prop with a curved parkerised steel shell, stamped ribs, a green tape wrap, a rubber pull loop, and recessed brass details. The magazine is posed on its side.

| Specification | Final value |
| --- | --- |
| Blender bounds (X × Y × Z) | 0.3266178 × 0.1029421 × 0.0299606 m |
| Triangle count | 2,980 |
| Materials | 1 |
| Mesh object | `Body` |
| Origin | `(0, 0, 0)` at the bottom centre of the footprint |
| Transform | Identity rotation and unit scale |
| UVs | One non-overlapping UV0 atlas |

In Blender, +Z is up and +X points along the body from the pull loop toward the brass display end. The GLB uses the Blender exporter's default glTF +Y-up conversion and metre units. The tape, pull loop, and decorative brass details are part of the single closed `Body` mesh.

## Files

- [WeaponPart_ExtendedMagazine.glb](WeaponPart_ExtendedMagazine.glb) contains the mesh, material, and all three texture maps.
- [BaseColor](WeaponPart_ExtendedMagazine_BaseColor.png) is sRGB surface colour with lighting and ambient occlusion excluded.
- [Normal](WeaponPart_ExtendedMagazine_Normal.png) is a linear tangent-space OpenGL (+Y) map.
- [ORM](WeaponPart_ExtendedMagazine_ORM.png) is a linear packed map with occlusion in red, roughness in green, and metallic in blue.

The texture maps are original procedural artwork at 1024 × 1024 pixels.

## Previews

All five 1600 × 1200 previews were rendered in Cycles from a fresh Blender GLB re-import. The scale image includes a measured 1.8 m grey figure.

- [Front](preview_front.png)
- [Side](preview_side.png)
- [Three-quarter](preview_34.png)
- [Close-up](preview_close.png)
- [Scale](preview_scale.png)
- [Contact sheet](contact_sheet.jpg), 2 rows × 3 columns, 2374 × 1202, JPEG quality 85

## Review and validation

The first full render washed out the parkerised finish, showed a visible discontinuity across the tape wrap, and made the stamped ribs look shallow. After review, the studio lighting was reduced, the tape edges were aligned across UV seams, and the ribs and rubber plate were given more relief. A second review tightened the edge abrasion and clarified the brass and rubber silhouettes.

The final GLB was checked for outward winding, closed manifold topology, unit transforms, bottom-centre placement, non-overlapping exported UV0 triangles, and pixel agreement between embedded and standalone textures. Blender 5.3 Beta re-import and all five preview renders passed. All brief deliverables are complete. Import into Unreal Editor remains untested.

## Rebuild

From this folder, run the following with Python 3, NumPy, and Pillow installed. The script uses the installed Blender 5.3 Beta in background mode, generates the maps and mesh, verifies the GLB, and renders the previews and contact sheet.

```powershell
python -B build_asset.py
```

Use `--blender "C:\path\to\blender.exe"` to select another Blender 5.x executable. Rebuild sources are [build_asset.py](build_asset.py), [blender_asset.py](blender_asset.py), and [asset_layout.py](asset_layout.py).
