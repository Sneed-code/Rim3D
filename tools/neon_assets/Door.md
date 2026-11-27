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

# Door (budget 6k tris, 2048 textures)
Game use: Door, the sliding leaf of a colony door. The game draws the door frame itself and slides THIS leaf sideways 0.85 m along X into the wall to open it, so make only the leaf: a slab exactly ~0.74 m wide (X) x 0.10 m thick (Y) x 2.15 m tall (Z), bottom at z = 0, centred on the origin; nothing may stick out past 0.05 m from the centre plane in Y (no protruding knob or hinges; it slides into a wall pocket). A sturdy hand-built frontier door for a tropical jungle colony: a heavy rail-and-stile frame with recessed panels or vertical tongue-and-groove planks, diagonal or Z bracing on both faces, steel edge banding and corner plates with rivets/bolt heads, a recessed pull handle (finger pocket or flush D-pull) on both faces near the leading edge (+X side, the side that faces the opening), a small louvred vent panel low down for airflow in the humid heat, kick plate at the bottom, scuffs at hand and boot height, dents and chips on the edges. It is made of whatever material the player chooses (wood, steel, stone, plasteel): the game tints it, so keep the colour a fairly light, neutral, near-uncoloured tone (light grey-beige wood grain on the panels, slightly darker neutral grey on the metal fittings) with the wear and detail carried mostly in the normal and roughness maps. Both faces equally finished.
Separate parts: `Body`.
Axis: building convention, front facing -Y, origin at the bottom centre of the leaf.

## No animation
This asset has no moving parts and no animation: skip `preview_anim.gif` and say so in README.md. Spend the time on shape, wear and material quality instead.
