# SunLamp

SunLamp is a six-optic indoor crop light for a 1 × 1 m Rim3D cell. The weighted steel
foot, telescoping mast, cast yoke, finned reflector and side-vented ballast are modeled
at real scale.

| Specification | Value |
| --- | --- |
| Blender bounds (X × Y × Z) | 0.998 × 0.892 × 2.391 m |
| Triangles | 5,988 of 6,000, with 5,400 in Body and 588 in Bulbs |
| Materials | Industrial surfaces and emissive grow optics |
| UVs | One non-overlapping UV0 atlas shared by both meshes |

## Parts and pivots

| Mesh | Blender-space pivot in metres | Role |
| --- | --- | --- |
| `Body` | (0.000, 0.000, 0.000) | Cell centre at floor level. The mast and reflector are in their deployed pose. |
| `Bulbs` | (0.000, -0.057, 2.186) | Centre of the optic bank at its seating plane, for swapping the six optics as one assembly. |

Blender uses +Z up and −Y toward the working side. The default Blender glTF
export maps (X, Y, Z) to (X, Z, −Y), so `SunLamp.glb` uses +Y up and +Z
toward the working side. Both coordinate systems use metres. The geometry
is triangulated and the object scales are 1. The reflector is posed for the asset
and the bulb assembly is the separate replaceable mesh.

## Textures

All three 2048 × 2048 PNGs are included separately and embedded in `SunLamp.glb`.

| File | Color space and channels |
| --- | --- |
| `SunLamp_BaseColor.png` | sRGB pigment with no baked lighting, shadows, or AO. The optics material also uses this image as its emissive color. |
| `SunLamp_Normal.png` | Linear OpenGL tangent-space normal (+Y). Flip the green channel if a manual Unreal texture setup expects the DirectX convention. |
| `SunLamp_ORM.png` | Linear packed map. Red is local occlusion, green is roughness, and blue is metallic. |

The optics use `KHR_materials_emissive_strength` at 2.25. In game, use a light component
for crop illumination when the gameplay or lighting setup calls for it.

## Preview and art review

The five 1600 × 1200 PNGs and `contact_sheet.jpg` show the exported GLB re-imported
into a clean Blender scene. The scale frame includes a 1.800 m matte grey mannequin.
The previews were rendered in Cycles.

The first full render showed the yoke intruding into the reflector, exposed ends
at the ballast louvers, an abrupt studio horizon, and featureless white optics.
The second pass revised the yoke clearance and cable guides, resized and redetailed
the ballast vents, and added readable microprisms. It also refined the finish wear,
added the foot service plate and lifting pockets, and replaced the backdrop with
a continuous sweep.

## Build

Run `python build_sunlamp.py` in this folder with Blender 5.x, Python, NumPy and
Pillow. To select a Blender installation, add `--blender "path/to/blender.exe"`.
The build uses local procedural geometry and textures, exports with Blender's
default glTF settings, renders a fresh import, and validates the final files.

All brief deliverables are complete. Runtime collision, a light Blueprint, and
animation are outside the delivered GLB. Validation covered the exported binary
and the Blender round trip. Unreal import is unverified.
