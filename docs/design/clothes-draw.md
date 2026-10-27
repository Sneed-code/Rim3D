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
