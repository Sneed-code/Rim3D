# Scanned skin (MetaHuman scans on the MakeHuman bodies)

sneed, 2026-11-28: "so we'd use metahuman and do all the stuff you said ... i assume this is the best skin option?"
MakeHuman's skins are painted 2K textures (`makehuman.md` "Skin realism"); the MetaHuman Creator plugin that ships with
UE 5.8 has real scanned skin for free: body colour maps and surface detail normals
(`/MetaHumanCharacter/Optional/BodyTextures`, about 600 MB) and a face texture synthesis model that makes a face's colour
and normal map for any Creator preset. This moves them onto the creator bases' own texture layout, so everything that
uses that layout keeps working: the pawn's skin colour (the material shifts the texture's average to it), the pigment
mask, wrinkles, markings, skin blood, the garment cover and the 4K switch.

Paid scans (Texturing XYZ, about US$50 to $150 a head) are the only clearly better source; free photo-to-texture AI
tools give colour only, no pore depth or shine.

## Steps (both automatic, `tools/art_steps.json`)

1. **`scan-skin-export`**, the `ScanSkin` commandlet (`game/Source/Rim3DEditor/ScanSkinCommandlet.cpp`, config
   `assets/scan_skin.json`) writes to `game/Saved/ScanSkin`:
   - `mesh_body.json` (the Creator's template body `SKM_Body`) and `mesh_face.json` (Kellan's face mesh): points,
     triangles, texture coordinates, material slots, skin weights and the skeleton's bind pose, in Unreal's terms;
   - `textures/*.png`, every texture under `textureFolders` from its source art, and `textures.json`, a list of every
     texture under `listFolders` (to pick from);
   - `face_<Preset>_albedo.png` and `_normal.png` from the texture synthesis model (the biggest one the plugin ships)
     for each preset in `faceSynth`, as PawnFaces makes the face skins.
2. **`scan-skin-bake`**, `tools/makehuman/scan_skin.py` (system Python; it installs OpenCV contrib and downloads the
   54 MB LBF landmark model the first time) runs Blender twice per base (`scan_skin_blend.py`):
   - **Skeleton line-up**: the MetaHuman parts are turned, moved and scaled onto the base by the two skeletons
     (pelvis, head, hips; a mirror is caught by the toes), then every MetaHuman bone is moved onto the matching
     MakeHuman joint (the torso and neck by share of the chain: MetaHuman has five spine and two neck bones, the
     game rig three and one; the hands rolled by the knuckles) and the skin carried by its own weights.
   - **Face landmarks**: both faces drawn from the front (Cycles, orthographic); OpenCV's LBF model finds 68 points on
     each; the 33 of the nose, eyes and outer lips are cast back onto the meshes; a similarity (refitted three times
     without far misses), then a thin-plate spline through what is left, both fading out below the chin.
   - **Settle**: each MetaHuman point moves to the nearest base skin point facing the same way (within 7 cm), the
     moves smoothed over the surface.
   - **Bake** (Cycles, selected to active, 2.5 cm rays): coverage, which part (face or body), colour, normal
     (Unreal's green-down normal maps flipped for Blender and back), roughness and cavity, 4096 px. The face is
     brought to the body's tone at the neck; where the scan doesn't reach (inner lips, genitals, horns' bases) the
     base's own 4K maps stay, tone-matched. Roughness is centred on mid-grey (the material's own value); cavity makes
     pores a little rougher and is written as `skin_scan_cavity.png` too.
   - Out: `skin_scan.png`, `skin_scan_normal.png`, `skin_scan_rough.png` beside each base's FBX; pictures of each
     step (faces with landmarks, coverage) and Blender logs in `game/Saved/ScanSkin/work`.
3. **`makehuman-bodies`** (MakeHumanArt) uses `skin_scan*.png` for `MI_MH_<base>_Skin4K` when they are there,
   `skin_4k.png` otherwise.

Which texture is which goes in `assets/scan_skin.json` `bases` (an exported texture by asset name, or
`synth:<Preset>:albedo|normal`); `"colour": "detail"` keeps MakeHuman's colours and only adds the scan's fine detail.
`SCAN_SKIN_SIZE=1024` for a quick try.

## Tested

The whole bake on a stand-in (MakeHuman's CC0 base mesh as both the base and a reposed, 4% taller "MetaHuman" with a UV
gradient texture, Blender 5.2's Python module): landmarks found on both faces, 32 of 33 pairs kept, the baked colour
within 0.2% of the gradient over 72% of the texture. `tools/tests/test_scan_skin.py` covers the line-up maths.

## Next

- One texture set per base for now (the material recolours it). Light, medium and deep sets picked by tone if
  undertones look flat.
- Nipple landmarks from the pigment mask if the scan's areolae land off MakeHuman's.
- Peach fuzz.
