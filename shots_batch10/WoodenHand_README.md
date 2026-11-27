# WoodenHand

A palm-down wooden prosthetic for Rim3D. The carving has four partly fused, curled fingers and a separate thumb silhouette. A split leather cuff, crossed cord lacing, and two buckled straps fasten the wrist.

| Property | Value |
| --- | --- |
| Dimensions in Blender X × Y × Z | 0.240 × 0.100 × 0.060 m |
| Exported triangles | 2,980 |
| Mesh and pivot | `Body`, at the bottom centre of the complete asset |
| Materials | One opaque PBR material |
| UVs | One non-overlapping UV0 atlas |
| Blender axes | +X towards fingertips, +Z up, palm down |
| glTF axes | +X towards fingertips, +Y up, metres |

## Textures

All three 1024 × 1024 maps are embedded in `WoodenHand.glb` and supplied as separate PNGs.

- `WoodenHand_BaseColor.png` · sRGB pigment and surface wear.
- `WoodenHand_Normal.png` · tangent-space OpenGL (+Y). Flip the green channel if importing this PNG independently into Unreal's DirectX normal workflow.
- `WoodenHand_ORM.png` · linear data. Red is ambient occlusion, green is roughness, and blue is metallic.

## Previews and review

The five studio views were rendered after re-importing the GLB into a clean Blender scene. The scale view includes a 1.80 m reference. The contact sheet includes all five angles.

The first renders washed out the wood grain, made the leather and steel read too similarly, and exposed raised lace ends. The final pass rebalanced the studio lighting, shaped hand-cut planes into the wood, varied the oil finish and knuckle wear, seated the laces, and added wear around their punched holes. All brief deliverables are complete. Validation covers the exported GLB and its Blender re-import.

## Rebuild

Run `python build_woodenhand.py` from this folder with Python, NumPy, SciPy, scikit-image, Pillow, and Blender 5.x installed. Set `WOODENHAND_BLENDER` to the executable path to select a different Blender 5.x installation. The build uses `woodenhand_blender.py` for mesh assembly, GLB export, and studio rendering.
