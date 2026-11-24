# Shared brief for every Rim3D Codex asset (read this first)

You are making ONE finished, game-ready, photoreal 3D asset for Rim3D, a 3D colony survival game in
Unreal Engine 5.8 (realistic look, like a modern AAA survival shooter; NOT stylised, NOT low-poly cartoon).
Setting: a tropical frontier world in the year 6650; tech is a mix of rugged industrial and worn spacer
gear built and repaired by colonists. Everything should look used: scuffs, chipped paint, dirt in
crevices, sun-faded paint, light rust on steel, finger grease on handles. Original design (do not copy any
existing game's art or any trademark/logo). No text except small generic stencils/warning stripes.

Work only inside the current folder. Use Blender 5.x from the command line (`blender -b --factory-startup
-P <script>.py`; find blender.exe under C:\Program Files\Blender Foundation\ if it is not on PATH) and Python
(numpy/Pillow) to model, UV, texture and export. Keep your build script in the folder so the asset can be
remade.

## Must deliver (in this folder)
1. `<Name>.glb`: one glTF binary exported with Blender's glTF exporter at its default settings (it writes the file Y-up, as glTF requires; do NOT turn off +Y up), metres, +Z up in Blender, real-world scale (check it with a 1.8 m reference human
   while you work), textures embedded.
2. PBR textures, made procedurally or painted by you (no downloaded textures): BaseColor (sRGB, NO baked
   lighting, NO baked shadows or AO in the colour), Normal (OpenGL / +Y), and ORM packed (R = ambient
   occlusion, G = roughness, B = metallic). 2048x2048 unless the brief says otherwise; one material per
   asset if possible, at most three.
3. Clean, non-overlapping UVs in UV0 (lightmap UV1 not needed). Real modelled shape and bevels: silhouettes
   and edges must read up close, not a box with a picture on it. Normal map for small detail
   (screws, panel seams, weld beads, knurling).
4. Separate named mesh objects for anything that moves or is swapped (names given in the brief); each moving
   part's origin at its real pivot. Everything else joined into the main body object.
5. Triangle budget as given in the brief; no hidden internal faces, no n-gons, consistent outward normals,
   applied transforms (scale 1).
6. Previews rendered from the GLB as re-imported into a fresh Blender scene (Cycles or EEVEE, neutral grey
   studio, soft key light + HDRI-ish sky light, 1600x1200): `preview_front.png`, `preview_side.png`,
   `preview_34.png` (three-quarter from above-front), `preview_close.png` (detail close-up), and
   `preview_scale.png` with a plain 1.8 m grey human silhouette next to the asset. Then
   `contact_sheet.jpg`: all five in one 2x3 grid, quality 85, max 2400 px wide.
7. `README.md`: final size in metres (x, y, z), triangle count, texture list, part names and pivots, the
   axis convention used, and anything not finished.

## Quality bar
Before finishing, look at your own previews critically, as a picky art director would, and fix what looks
fake: flat untextured areas, stretched UVs, floating parts, wrong scale, plastic-looking metal, missing
bevels, baked shadows in the colour. Do at least one improvement pass after the first full render.

## Axis conventions
- Hand weapons: +X out of the muzzle, +Z up, the firing hand's palm on the grip at the origin (0,0,0), the
  weapon's right side towards -Y in Blender. Thrown items: origin at the centre of mass.
- Buildings, turrets and big props: footprint centred on the origin, bottom at z = 0, the front (the side a
  colonist works at, or where the barrel points at rest) facing -Y in Blender. Footprint cells are 1 m.
- Small items lying on the ground: origin at the bottom centre.

---

# Spelopede (budget 20k tris, 2048 textures)
Game use: the chitinid stalker, a mid-sized chitinid (insect-like creatures once engineered to break up rock). A
long, low, fast centipede-beetle hybrid about 1.6 m long, 0.6 m high: a flat armoured head with long curved
pincer-mandibles and long antennae, an elongated body of overlapping plates, many fine legs (8-10 pairs) along the
body, a whip-like tail tip. Ochre-yellow and dark brown chitin, glossy, scratched and dusty, pale joint membranes.
Realistic; unsettling. Original design.
Bones: a spine chain for the body segments so it undulates, each leg pair rigged.
Speeds: Walk 1.6 m/s, Run 4.2 m/s (legs ripple in waves, body snakes slightly). Attack: a lunge with the pincers
snapping shut.

## Creature rules (this one is a rigged, animated creature, not a building)
These override the building/hand-weapon parts of the shared brief:
- One `Armature` with a clean bone hierarchy (root bone `Root` at the ground under the body centre, then
  `Body`/`Pelvis`, spine/segments, every leg, arm, blade, head and gun as its own bone chain), and ONE skinned mesh
  object parented to it (armature modifier). Hard-surface parts may be weighted 100% to a single bone (rigid
  skinning is fine and preferred for armour plates); soft joints (cables, hoses, insect joints) may blend two bones.
  No shape keys, no constraints or drivers left in the export (bake them into the actions), no IK bones needed at
  runtime. Max 4 bone influences per vertex, at most ~80 bones.
- Real size in metres, standing in its rest pose with feet on z = 0, facing **-Y** in Blender (the glTF exporter makes
  that the file's +Z forward; say in README which way it faces). Origin and `Root` at the ground under the body.
- The actions below, each its own glTF animation (Blender actions pushed to NLA tracks, exported by the glTF exporter,
  "Animation > Group by NLA track" default), named exactly as written, 30 fps. Loops must loop seamlessly (first
  frame = last frame pose). Walk/Run are **in place** (the root bone does not travel; the game moves the creature):
  feet plant and push back with no sliding against the speed given, so the game can match it.
  - `Idle`: 4 s loop: breathing/servo sway, a small head or sensor turn, weight shift.
  - `Walk`: one full stride cycle in place, at the walk speed given below.
  - `Run`: one full stride cycle in place, at the run speed given below.
  - `Attack`: about 0.9 s, its melee strike (or the brief's attack), starting and ending in the Idle pose.
  - `Hit`: 0.4 s flinch, back to Idle pose.
  - `Death`: about 1.5 s collapse onto the ground (body ends lying still on z = 0, nothing below the ground), held on
    the last frame.
- Render `preview_anim.gif` (three-quarter view, 800x600, every action one after another, each labelled in the corner)
  and check it: no feet sliding, no parts flying apart, no mesh tearing at joints, death ends on the ground.
- Test the export: re-import the GLB in a fresh Blender, check the armature, skin and all six actions are there and
  play, and render the previews from that re-import. List bones, actions (frames, length) and the walk/run stride
  length per cycle (metres) in README.md.
- Triangle budget as given; 2048 textures (BaseColor, Normal, ORM, plus Emissive for eyes/lights), one material if
  possible.
