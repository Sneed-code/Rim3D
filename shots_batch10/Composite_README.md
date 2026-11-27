# Composite

A Rim3D stack item made of four bevelled fibre-reinforced panels. The charcoal twill sits beneath a semi-gloss resin surface. The cut edges reveal pale laminate plies, the second panel from the top is shifted 10 mm along +X, and a continuous woven band wraps the bundle. The upper face carries light handling scuffs and a partial dusty fingerprint.

## Mesh and placement

| Property | Value |
| --- | --- |
| Final size in Blender X × Y × Z | 0.4100 × 0.3022 × 0.06085 m, including the band |
| Triangle count | 960 |
| Mesh and pivot | `Body`. Bottom centre at `(0, 0, 0)`, with unit scale and identity rotation |
| Parts | Static stack with four closed panel shells and one closed band shell in `Body` |
| Material | One material named `Composite` |
| UVs | Single non-overlapping UV0 atlas |
| Axes | Metres. Blender uses +Z up and −Y for the front view. The default Blender glTF export uses +Y up and +Z for that same front view. |

## Textures

Each procedural map is 1024 × 1024. The PNGs are supplied separately and embedded in [Composite.glb](Composite.glb).

| Texture | Encoding |
| --- | --- |
| [Composite_BaseColor.png](Composite_BaseColor.png) | sRGB material colour and wear markings, free of baked lighting or AO |
| [Composite_Normal.png](Composite_Normal.png) | Linear tangent-space normal, OpenGL +Y |
| [Composite_ORM.png](Composite_ORM.png) | Linear. R carries ambient occlusion, G roughness, and B metallic |

## Previews and review

The five 1600 × 1200 previews were rendered from the exported GLB after re-import into an empty Blender scene. The scale view includes a 1.800 m grey standing reference.

[Front](preview_front.png) · [Side](preview_side.png) · [Three-quarter](preview_34.png) · [Close-up](preview_close.png) · [Scale](preview_scale.png) · [Contact sheet](contact_sheet.jpg)

The first full set showed a pale face, paper-like banding and an overly uniform fingerprint. The revision darkened and flattened the twill beneath the resin, adjusted the lighting to show the sheen at a grazing angle, added a visible welded patch to the band and broke up the fingerprint ridges. Fine abrasions and ply variation are visible in the close-up.

## Rebuild and verification

Run `python build_composite.py` from this folder with NumPy, Pillow and Blender 5.x installed. Use `--blender <path>` to specify a different Blender executable. The script generates all three PNG maps, the GLB, five previews and the contact sheet. Use `--draft` to reproduce the first review in a separate folder.

Verified the GLB's dimensions, unit transform, outward winding, closed shells, non-overlapping UV0, triangle count and embedded image contents. All requested deliverables are complete. Import verification was performed in Blender 5.3 Beta. Unreal Engine 5.8 import remains untested.
