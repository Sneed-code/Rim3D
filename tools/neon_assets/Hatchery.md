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

# Hatchery (budget 8k tris, 2048 textures)
Game use: Hatchery, a heated box that keeps fertilized eggs warm until they hatch, one 1 x 1 m cell,
~0.8 m tall. A sturdy wooden or sheet-metal cabinet on short legs with a hinged glass-topped lid, inside a
bed of straw holding a few eggs, a warm orange emissive heat lamp under the lid, a small thermostat dial
and a power cable out the back. Chipped paint, straw sticking out of the lid seam, a few feather bits.
Separate parts: `Body`, `Lid` (pivot on its hinge at the back). Empty `Eggs` at the egg bed centre.
Emissive map for the heat lamp.
Axis: building convention, the front facing -Y.

## Animation (this asset must move)
Extra parts: make the heat lamp bulb a separate object `HeatLamp` (own warm orange emissive material) under the lid, and one egg a separate object `Egg1` (pivot at its bottom).
- `LidOpen`: `Lid` swings open 75 degrees on its back hinge in 0.8 s (ease in/out), holds; `LidClose` the reverse.
- `Incubate`: seamless 3 s loop: `HeatLamp` pulses in scale 1.0 to 1.06, and `Egg1` wobbles a few degrees side to side twice near the end of the loop (about to hatch).
- Put each action in the GLB as its own glTF animation (Blender actions pushed to NLA tracks, exported by the glTF exporter), named exactly as above, 24 fps, linear interpolation for loops so they loop seamlessly.
- Every moving part named in the brief must be its own mesh object with its origin at the real pivot, and the actions must move only those parts (never the main body).
- Render `preview_anim.gif`: three-quarter view, 800x600, 24-48 frames covering the actions one after another (or the loop), looping, so each motion is clearly visible. Check it plays.
- List the actions, their frame ranges, lengths and the frame rate in README.md.
