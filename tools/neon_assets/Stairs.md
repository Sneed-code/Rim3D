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

# Stairs (budget 8k tris, 2048 textures)
Game use: Stairs, climbing one 3 m storey over three 1 m cells (footprint exactly 1.0 m wide in X by 3.0 m long in Y, height 3.0 m). The foot is at the front (-Y end, y = -1.5) and the top step at the back (+Y end, y = +1.5, top surface at z = 3.0). Exactly twelve steps, each rising 0.25 m with a 0.25 m tread: step k (k = 1..12) has its tread top at z = 0.25 k and spans y from -1.5 + 0.25 (k-1) to -1.5 + 0.25 k. Colonists walk on these exact treads, so keep them flat and at those heights. A sturdy hand-built frontier staircase for a tropical jungle colony: two heavy side stringers (notched or closed) running the full length, thick treads with a slightly rounded nosing overhanging a few cm, closed risers, the space underneath filled in as a solid stepped block (no see-through gaps), a simple low handrail on ONE side (+X side) on three or four posts, staying inside the 1 m footprint. Worn dips in the middle of the treads, scuffed nosings, chipped corners, dirt in the corners of each step. It is made of whatever material the player chooses (wood, steel, stone): the game tints it, so keep the colour a fairly light, neutral, near-uncoloured tone (light grey-beige) with the wear, grain and joinery detail carried mostly in the normal and roughness maps.
Separate parts: `Body`.
Axis: building convention, front (the foot) facing -Y, origin at the footprint's floor centre.

## No animation
This asset has no moving parts and no animation: skip `preview_anim.gif` and say so in README.md. Spend the time on shape, wear and material quality instead.
