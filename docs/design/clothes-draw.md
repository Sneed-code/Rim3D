# Clothes on pawns

Code: `game/Source/Rim3D/PawnActor.*` (`ApplyApparel`, the skin layer, the worn meshes), `PawnSubsystem.*`
(`MakeApparel`, `ApparelKey`, `GetClothesAsset`), `Rim3DEditor/ClothesArtCommandlet.cpp` (the garment
material and the bare skin meshes). Smoke test: `clothes` (`tools/smoke.ps1 -Test clothes`). Task U16. Unreal
side only; the sim ([apparel.md](apparel.md), C15) is untouched. The meshes are A5's ([looks.md](looks.md)).

**The sim decides, Unreal shows.** What a pawn wears is `ThingView::firstApparel` and `apparelCount` into
`Snapshot::apparel` (item, def, stuff, `hitPointShare`). Unreal draws exactly that list and nothing else; taking
clothes off, stripping, a piece wearing out (`ApparelDestroyed`) and putting on a new one are all just the list
changing.

## What is on a MetaHuman

Kellan's MetaHuman body only has hands and feet; his hoodie and trousers are what cover the rest. So:

1. **Kellan's own top and trousers are hidden** (the shoes stay: the sim has no footwear yet).
2. **A skin layer** is always drawn: `SK_Body_Bare_0` (torso and arms) and `SK_Body_Bare_1` (legs), copies of the
   hoodie and cargo trousers (hood and hands cut away, 1.2 cm slimmer) on the garment material, coloured with the
   pawn's skin (`FSimPawnLooks::Skin`). Where nothing is worn this is what shows. With the PawnFaces assets it is drawn
   with `M_PawnBareSkin` instead (V2b, [looks.md](looks.md)): lit like the face and hands, in the pawn's skin colour, with
   a fine tiled skin normal (the skin texture's UVs do not fit these meshes). The shape is the loose hoodie's, so a bare
   pawn looks a little padded.
3. **Each worn piece** puts its `visual.worn` meshes on over the skin, in the order the sim lists them (the order
   put on; the garments' thickness grows by layer, A5): a skinned mesh follows the same leader pose as the
   hoodie did and gets the same body-shape morphs (`Thin`, `Heavy`, `Female`...), so it fits every body type; a
   mesh with a `bone` (hats) is attached to that bone of the MetaHuman's body.
4. **Hair** hides while a piece with `apparel.hidesHair` (tuque, simple helmet) is worn; the cowboy hat lets it show.

Since the real body ([looks.md](looks.md), character creator step 1) the MetaHuman's own body is the engine's whole body,
all skin, so steps 1 and 3 stay but there is no skin layer (step 2): where nothing is worn, and through torn holes, the
body shows. The skin layer is only made for older generated assets.

If the clothes assets (or the looks assets) are missing, Kellan's own clothes stay and nothing else changes.
Mannequins have no clothes. A pawn's meshes are remade only when the list's items, defs or stuffs change.

## Tint and tear

One material, `M_Garment` (made by the ClothesArt commandlet), for everything: base colour, a mask and a custom
primitive data input, so no material instance is made per pawn. Index 0 to 2 is the cloth's colour, 3 the wear.

- **Tint**: a piece made of a stuff takes the stuff's `visual.color` (cloth, leather, devilstrand...); a piece with
  none (the flak vest) keeps its own colour.
- **Torn look**: wear is 0 down to 70% of the hit points and grows to 1 at none, in sixteenths. The material cuts holes
  where a noise on the garment's UVs is under 55% of the wear (the skin layer shows through), edges them dark, and
  darkens the whole cloth by up to 30%. A piece at 5% is mostly holes. Hit points come from the sim's wear and tear
  and from armor absorbing damage (C15), so cloth that has been shot looks it.
- **Destroyed**: the sim takes the piece off the list; its meshes go the same frame.

## MakeHuman bodies (2026-11-20)

The MakeHuman people (the creator's women and men, `docs/design/makehuman.md`) wear real clothes: free MakeHuman
community garments (`.mhclo`, CC0 and CC-BY, list and authors in `assets/makehuman_garments.json`) fitted to the base
body by MPFB.

- **Making them**: `python tools/makehuman/mh_garments.py` (it runs Blender once per base) fits every garment to the base,
  builds the same slider variants as the body so each garment gets **the same morph targets** (a fat, thin, tall or
  muscled body moves its clothes with it), and writes `game/Saved/MakeHuman/<Base>/Garments/<G>/<G>.fbx` plus
  `cover.png`/`cover.json`. `UnrealEditor-Cmd ... -run=MakeHumanArt -Garments` imports them as `SK_MHG_<Base>_<G>` on the
  body's skeleton with one `M_MH_Garment` instance per slot.
- **Women's and men's cuts**: a garment with `bases` is made for that base only (blouse for women; polo and untucked
  shirt for men), and a few have a per-base `.mhclo` (tees, tank, trousers, jeans, uniform jacket, tactical vest).
- **Cuts**: a def's `visual.cuts` lists the looks a piece can have; `+` joins garments drawn together
  (`"VikingTunic+VikingPants"`). Each item picks one by its id among the cuts its wearer's base has, so the same
  T-shirt always looks the same and a colony in one fabric still looks varied. `check_defs.py` checks the names.
- **Colour is a look, not the stuff** (sneed 2026-11-20): a made-of-stuff garment takes one of 16 clothing colours
  (navy, olive, khaki, greys, browns, denim, maroon...) picked by its item id, over the garment's own shading (its light and dark capped at 1.4x the average, so a bright print or stripe stays cloth, not a pale patch). The stuff
  only changes the numbers. Gear without a stuff (flak) keeps its own colours. A dye or recolour choice is in the backlog.
- **Worn look and wet**: `M_MH_Garment` keeps the torn holes and darkening from the wear (custom data 3) and the
  wetness of the wearer (12).
- **Layers**: all garments are made to sit on the skin, so a top is pushed out along its normals over trousers (1.5 cm
  for a piece over the torso, 0.8 cm for legwear so its waistband covers the skin kept above it, 0.6 cm more per layer above the skin; custom data 5, `FSimWornPiece::Puff`).
- **Following the body**: each garment is a skinned mesh component on the body's leader pose; the body's morph weights
  are copied onto it when they change.
- **Hats**: a drawn overhead piece switches the strand hair to its under-a-hat groom (pressed flat above the brim, hanging
  below it; `WearsDrawnHeadwear`); a `hidesHair` piece hides the scalp hair sections.

### Skin under clothes

Skin poking through tight clothes when the body bends is cut away instead of pushed in. `mh_garments.py` casts a ray
along each body normal and marks the faces a garment covers within 3 cm, then keeps only skin more than 4 cm from any uncovered skin, so the skin at a hem or between a top and trousers stays drawn even when the cloth shifts a little in play (no holes through the body), and headwear (lowest point above the chin line) hides no skin; each garment that covers 40 faces or more gets
one bit (up to 31) in a per-base cover texture on the body's UVs (`T_MHG_<Base>_Cover`, 32 bits in RGBA8, bit 31 the crotch, nearest
filtering). The garment keeps its bit as `UGarmentCoverData`. While worn, the pawn sets the bits on the body (custom
data 30 low 16, 31 high 16) and the skin material (masked) clips the skin where a worn bit is set.

The crotch (genitals, pubic area and inner thighs: a box round the crotch plus the `genital` vertex group) stands out
past trousers fitted to the basemesh, so it has its own bit, 31, set whenever a piece with `bCoversCrotch` is drawn.
Pubic hair (its own
material sections) is hidden while a piece over the legs on the skin or middle layer is drawn (`bCoversCrotch`).

## Cost

12 pawns with 3 pieces each: 4 ms game thread at 888 x 500, 2.5 to 4 ms GPU. 100 pawns in a shirt and trousers
(`clothes_crowd_100`): 13 to 15 ms game thread, 6 to 9 ms render thread, 64 fps (the 100 shaped MetaHumans of the `looks`
test, with Kellan's own clothes, measured 18 ms game thread on a different scene). Every worn mesh is a skinned mesh
component of its own, so a pawn with 4 pieces costs about 7 components; the clothes are not yet dropped at a distance
(a later pass can hide the skin layer and garments of far pawns).

## Not done

Boots, gloves, belts and backpacks (no defs); a sleeve or leg that the sim's coverage says is bare but the garment
mesh covers (the meshes are A5's approximations: a duster is thickened trousers, tribalwear a tunic and shorts); a skin
texture on the bare layer; dead man's clothes (tainted) have no look of their own; wet and bloody clothes; clothes on
animals; faces and hair styles are still Kellan's.

## Smoke test `clothes`

Twelve pawns in a row (a naked one, the A5 pieces in eleven combinations, women and men) on a flat map at midday
(`Sky.Hour 11`). Every frame it checks each pawn's actor against the snapshot: Kellan's top and trousers hidden, the two
skin layers there and in the pawn's skin colour, exactly the meshes the worn defs need (a skinned mesh follows its
leader pose and has the pawn's morph weights; a hat is on the `head` socket), the wear in the custom data equals the
hit points' wear, and hair hidden exactly when a covering hat is worn. Then it wears pieces down (the worst piece
draws at wear above 0.9), takes a duster off, strips a pawn and wears a tuque out (their meshes go, hair returns),
spawns a crowd of 100, and switches to mannequins (no clothes, nothing breaks). Screenshots in
`game/Saved/Smoke/clothes/`.

To make the assets again: `python tools/clothes_art.py` (hat download), then
`UnrealEditor-Cmd.exe game\Rim3D.uproject -run=ClothesArt -unattended -nosplash -AllowCommandletRendering`
(about 1 minute, after the PawnLooks commandlet). Run it again after pulling U16: it adds the skin meshes and the
torn-look material.
