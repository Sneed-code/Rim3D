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

# WoodFiredGenerator (budget 15k tris, 2048 textures)
Game use: WoodFiredGenerator, a power generator that burns wood logs, 2 x 2 cells (1.9 x 1.9 m footprint), ~1.3 m
tall. A riveted cast-iron firebox with a small hinged door and a glass/grille window showing glowing embers, a short
steam/smoke chimney, a small steam engine beside it driving a big spoked flywheel (~0.9 m) and a belt to a copper-wound
dynamo with a cable out, pressure gauge, a few split logs stacked against the side. Soot, rust, scorched paint, oil
stains. Emissive for the embers/fire window. Front (firebox door) facing -Y.
Separate parts: `Body`, `Flywheel` (pivot on its axle), `Piston` (slides in its cylinder), `Door` (pivot on its
hinge), `Embers` (the glowing fire bed and window glow, own emissive material), `DynamoRotor` (pivot on its axle).

## Animation (this asset must move)
- `Running`: seamless 2 s loop: `Flywheel` turns 2 full turns, `DynamoRotor` 6 turns (belt ratio), `Piston` slides
  back and forth 4 times in step with the flywheel, `Embers` flickers (scale 0.95-1.05, uneven).
- `DoorOpen`: `Door` swings open 100 degrees in 0.6 s and holds; `DoorClose` the reverse.

- Put each action in the GLB as its own glTF animation (Blender actions pushed to NLA tracks, exported by the glTF exporter), named exactly as above, 24 fps, linear interpolation for loops so they loop seamlessly.
- Every moving part named in the brief must be its own mesh object with its origin at the real pivot, and the actions must move only those parts (never the main body).
- Render `preview_anim.gif`: three-quarter view, 800x600, 24-48 frames covering the actions one after another (or the loop), looping, so each motion is clearly visible. Check it plays.
- List the actions, their frame ranges, lengths and the frame rate in README.md.
