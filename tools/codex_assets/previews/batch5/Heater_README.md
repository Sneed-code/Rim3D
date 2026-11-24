# Heater

Floor-standing electric heater for Rim3D’s 6650 tropical frontier. The painted steel cabinet has a recessed front grille, four glowing heating loops, a pilot lamp, a temperature dial, roof louvers, side carry handles, and a stowed power lead with a plug. The design and textures were made procedurally for this asset.

| Specification | Final asset |
| --- | --- |
| Footprint | 1 × 1 m game cell |
| Size in Blender (X × Y × Z) | 0.9559 × 0.7624 × 1.1030 m, including handles and cable |
| Geometry | 4,516 triangles, one mesh, one material, one non-overlapping UV0 atlas |
| Body pivot | `(0, 0, 0)` m, centred on the footprint at floor level |
| Separate parts | `Body`. The dial, handles, elements and lead are fixed geometry in the mesh. |
| Axes | Blender uses +Z up and −Y forward. The default Blender glTF export is Y-up with +Z forward. Both use metres. |

## Files

`Heater.glb` is the game export. `Heater.blend` is the editable Blender 5.3 Beta source with packed textures. To inspect the source, select `Heater.blend` in Codex and choose **Open → Blender 5.3 Beta**. The Windows `.blend` association on this machine currently points to Blender 5.2.

All four maps are 2048 × 2048 PNGs and are embedded in the GLB.

| Texture | Contents |
| --- | --- |
| `Heater_BaseColor.png` | sRGB surface colour, paint wear, corrosion and markings. No baked lighting, shadows or AO. |
| `Heater_Normal.png` | Linear tangent-space normal map, OpenGL +Y. Includes panel seams, fastener slots, skid welds and dial knurling. |
| `Heater_ORM.png` | Linear packed map. R = ambient occlusion, G = roughness, B = metallic. |
| `Heater_Emissive.png` | sRGB emission mask for the pilot lens and heating loops, with glTF emissive strength 2.4. |

`preview_front.png`, `preview_side.png`, `preview_34.png`, `preview_close.png` and `preview_scale.png` are 1600 × 1200 Cycles renders from the re-imported GLB. The scale view includes a plain 1.8 m reference figure. `contact_sheet.jpg` combines the five views in a 2 × 3 grid at 2400 px wide, JPEG quality 85.

## Review and rebuild

The first five-view review showed bright roof reflections, a clean service panel and faceted element bends. The final pass softened the roof reflections, added dust, handle runoff and scuffs, and gave the grille brow and controls irregular heat staining. The elements gained rounder bends and cooler ends at the ceramic sockets. The build also removed 462 fully enclosed triangles. The exported GLB passed checks for triangle count, normal winding, UV0 overlap, and embedded image pixels. The first-pass contact sheet is available in `.heater-draft` for comparison.

Run `python rebuild_heater.py` with NumPy and Pillow installed to rebuild, audit and render all files. The script uses the installed Blender 5.3 Beta executable by default. Set `HEATER_BLENDER` to override it. Run `python rebuild_heater.py --draft` to regenerate the first pass in `.heater-draft`.

The asset and previews are complete. Import into a live Unreal Engine 5.8 project remains unverified.
