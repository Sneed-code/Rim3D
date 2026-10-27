# Seeded pawn looks

Code: sim `ColonySim/Looks.h`, `Private/Looks.cpp` (made in `Sim::giveLooks`, `Scenario.cpp`);
Unreal `game/Source/Rim3D/PawnLooks.h` (shared names), `PawnActor.cpp` (`ApplyLooks`),
`PawnSubsystem.cpp` (`MakeLooks`, the looks assets); the generator
`game/Source/Rim3DEditor/PawnLooksCommandlet.cpp`. Tests: `Tests/LooksTests.cpp`, smoke test
`looks`. Data: `data/defs/Lineages/`, `Rules` def `LooksRules`. Task A3.

Stage 0 found that stretching the MetaHuman makes the body poke through its clothes and that the
sample's skin material can't be tinted (`docs/decisions/0002`). Looks now come from real MetaHuman
body shapes as morph targets, on the body and the clothes alike, and our own skin material.

## In the sim

Every humanlike pawn (its def names no other `race.intelligence`) gets a `PawnLooks` component
when it is made, from a seed and its own random stream, so looks never change anything else:

| Field | What | From |
| --- | --- | --- |
| `lineage` | Lineages defName (RimWorld's xenotype) | the scenario's `lineage` or a roll from its `lineageWeights` (Crashlanded: Baseline); `SetLineage` |
| `female` | gender | the colonist's; a coin flip for other pawns |
| `body` | RimWorld's body types Thin, Average (Male or Female), Fat, Hulk | `bodyTypeWeights` |
| `weight` | -1 thin to 1 heavy, the body shape blend | the body type's `weightRange` |
| `height` | times 1.80 m | `heightMale`, `heightFemale`, `hulkHeightBonus` |
| `melanin` | 0 light to 1 dark | `melaninRange` |
| `skin` | the colour drawn | `skinColors` (by melanin), then the lineage's skin gene's `visual.skinColor` over it by `skinColorAmount`, darker for more melanin |
| `hair` | the colour drawn | `hairColors` by weight, or the lineage's hair gene's `visual.hairColor` |

Colonists use the colonist's seed (so the same colonist always looks the same), the hauling test
pawns the map seed and their number, pawns spawned by command the seed, step and entity. All the
numbers are our own (RimWorld takes body types from backstories and skin from genes; neither
gives numbers for 3D bodies), and nothing but the drawing reads them. The skin colours are
albedos for the MetaHuman textures, not RimWorld's sprite tints.

`SetLineageCommand{pawn, lineage}` (dev command, later a lineage graft) recolours a pawn; its body
stays. Looks are in the snapshot (`ThingView::looks` indexes `Snapshot::looks`), in `stateHash`
and in saves (format 16). Pawns from older saves have none and keep Kellan's look.

What Unreal draws is under `visual` on the lineage's **genes** (X1, [genes.md](genes.md)), not the lineage: the last gene with `skinColor`, `hairColor` or `horns` counts. Emberkin: red skin, dark hair, horns
(`length`, base `radius`, `spread` from the middle of the forehead, all metres; `color`).

### Faces, hair, beards, marks (V1)

The same component also holds the face and skin marks, from their own random stream (so the A3
fields of a seed did not move). All numbers are our own, in `LooksRules`. Nothing in the sim reads
them; V2 (faces and hair on screen) and V3 (body hair, scars, tattoos on screen) draw them.

| Field | What | From |
| --- | --- | --- |
| `faceShape`, `brow`, `nose`, `jaw` | 0 to 1 sliders, 0.5 the average face (Unreal turns them into face morph weights) | the average of two draws; men's brow and jaw shifted up, women's down (`faceGenderShift`) |
| `eyes` | colour | `eyeColors` by weight |
| `hairStyle` | name from `hairStyles` (14: Short, Buzz, Crew, SlickBack, Mohawk, Bald, Medium, Bob, Long, Ponytail, Bun, Braids, Curly, Afro) | separate male and female weights |
| `naturalBeard`, `beard` | name from `beardStyles` (None, Stubble, Moustache, Goatee, Short, Full, Long); women always None | `beardStyles` weights; the lineage's last gene with `visual.beard: false` (Beardless) draws None |
| `naturalBodyHair`, `bodyHair` | 0 none to 1 thick | `bodyHair` range by gender; a gene with `visual.bodyHair` (Furskin: 1) draws that |
| `tattoos` | list of marks | 18% of pawns have 1 to 3 (`tattoos`), on regions by weight, style an index into `tattoos.styles` |
| `scars` | list of marks | one for each Scar or OldWound a healed injury leaves (H1's `PawnScarred`), at most `scars.max` (24) |

A mark is `{region, style, u, v, size}`: the region is Face, Neck, Torso, LeftArm, RightArm,
LeftLeg or RightLeg (hands count as arms, feet and toes as legs, eyes, ears, nose and jaw as the
face); `u` and `v` (0 to 1) place it on that region's skin, `size` is a factor. A scar's style is
picked among the `scars.styles` that list the healed injury (Cut gives Slash or Puncture, Burn gives
Burn...), else among those that list none (Stitched). Scars on organs and bones leave the
injury (it is still a scar to the health system) but no mark, because nothing shows on the skin.
Marks stay if the injury is later removed or the part lost; that is rare and V7/V8 can tidy it.

`setLineage` draws `beard` and `bodyHair` again from the natural values, so a lineage graft is
undone cleanly. Looks are in the snapshot (`PawnLooksView` has all of the above), in `stateHash`
and in saves (section `looks` version 2). A pawn from an older save has an empty `hairStyle` and
average faces; Unreal keeps Kellan's face for those.

## The looks assets (generated, not in Git)

`UnrealEditor-Cmd.exe game\Rim3D.uproject -run=PawnLooks -unattended -nosplash -nullrhi` (about
3 minutes, no window) makes everything in `/Game/Rim3D/Generated/PawnLooks` from the Game
Animation Sample's Kellan and the engine's MetaHuman Creator plugin, and deletes what was there.
`-Probe` only logs the checks. It needs nothing downloaded and no Epic sign-in.

- **Body shapes.** The engine ships the 18 fixed MetaHuman body types as DNA files
  (`Engine/Plugins/MetaHuman/MetaHumanCharacter/Content/Optional/Body/FixedCompatibility`). Kellan's
  body is `m_med_nrw` (his vertices match its full-detail mesh exactly; the joints are read as
  rotators, which rebuilds his skeleton to 0.05 cm). For each of `m_med_unw`, `m_med_ovw`,
  `f_med_nrw`, `f_med_unw` and `f_med_ovw` the target body is taken off its own skeleton and put on
  Kellan's by its skin weights, so the morph is the shape without the proportions: the hidden
  mannequin drives the MetaHuman with the leader pose, so bones always have the mannequin's lengths,
  and height comes from scaling the whole pawn instead. Within about 7 cm of Kellan's head the
  morphs fade out, so the head (which keeps its shape) still meets the collar.
- **Morph targets** `Thin`, `Heavy`, `Female`, `FemaleThin`, `FemaleHeavy` go on copies of every
  mesh Kellan wears (`SK_Looks_*`: his body, which is only his hands and a little of his feet, the
  hoodie, trousers and shoes), on every LOD. A body vertex takes its own delta; a clothes vertex
  the average of the four nearest body vertices'. Largest moves: heavy 7.6 cm (belly), female 3.5 cm.
- **Clothes materials.** Kellan's clothes materials weren't marked for morph targets (they draw as
  the default grey material on a morphed mesh), so `M_PawnCloth_*` copies the base and
  `MI_PawnCloth_*` carry the original instances' values.
- **Our skin material** `M_PawnSkin`: Kellan's colour texture divided by its own average colour
  (measured per texture, `SourceAverage`) times the pawn's skin colour from custom primitive data
  0 to 2, so any colour (red too) keeps the texture's detail. `MI_PawnSkin_*` replace his body and
  head skin (the head's three LOD materials). Subsurface profile is Kellan's head one.
- **Parts.** `SM_PawnHorn` (a curved cone, 1 cm long, 0.1 cm base radius, scaled per lineage) and
  `M_PawnPart` (one colour from custom primitive data).

## Faces, hair and marks on screen (V2, V3)

Code: Unreal `game/Source/Rim3D/PawnFaces.h` (shared names), `PawnActor.cpp` (`ApplyFace`, `SetGroom`, `ApplyMarks`),
`PawnSubsystem.cpp` (`MakeLooks`, `GetFacesAsset`); the generator `game/Source/Rim3DEditor/PawnFacesCommandlet.cpp` and its
list `assets/pawn_faces.json`. Smoke test `faces`. Everything comes from the engine's own MetaHuman Creator plugin
(`MetaHumanCharacter`, enabled for the editor only in `Rim3D.uproject`): nothing is downloaded and nothing needs a sign-in.
The game reads only what the commandlet copies into `/Game`, so a packaged build does not need the plugin.

`UnrealEditor-Cmd.exe game\Rim3D.uproject -run=PawnFaces -unattended -nosplash -nullrhi` (about 2 minutes, after PawnLooks)
makes `/Game/Rim3D/Generated/PawnFaces` (about 370 MB, not in Git) and deletes what was there. `-Probe` only logs.

- **Face shapes.** The Creator's face model (its `Face/IdentityTemplate`, evaluated offline) gives the 29 preset faces
  (`presets` in the json, by gender) as moves from its average face, each lined up on its neck. Five morphs go on a copy of
  Kellan's face (`SK_Face_Kellan_FaceMesh`, every LOD): `FaceFemale` (the female presets' average minus the male ones'), and
  `FaceShape`, `FaceBrow`, `FaceNose`, `FaceJaw`, each the presets' main way of varying (first principal component, within
  each gender) inside that region of the face, 1.6 standard deviations at weight 1, positive bigger (wider face and jaw,
  nose and brow forward). The regions fade out under the chin, behind the cheeks and up the forehead, so the neck still
  meets the body and the scalp (which the hair is bound to) does not move. The model's vertices reach Kellan's lighter
  mesh by the four nearest of the same part (head, teeth, eyes, lashes...), after a scale and offset that fits the eyes and
  teeth (1.095; within 3 mm). Largest move of any morph: 1.4 cm. The sim's sliders drive them directly: weight = 2 x slider - 1.
  The log lists each preset's share of the female face so a mislabelled preset shows (the face model has no gender field).
- **Eyes.** Kellan's eye material copied as `M_PawnEye` (marked for morph targets, which it was not) and one instance per
  `LooksRules` eye colour, `MI_PawnEye_<sRGB hex>`, with the MetaHuman iris colour chart cell nearest that colour; `MI_PawnEye_Default`
  is Kellan's. A pawn's eye colour finds its instance by hex, so changing the colours needs the commandlet run again.
- **Hair, beards, brows.** For each of the sim's 14 hair styles and 7 beards the json names one of the Creator's grooms
  (Bald and None draw none; a beard is a beard groom and a moustache groom). The commandlet copies each as `GA_<Kind>_<Style>`
  (Hair, Beard, Moustache; brows as `GA_Brows_F0..` and `GA_Brows_M0..`), strands only (no cards or helmet meshes, which live in
  the plugin), on Kellan's hair material, no simulation, every LOD strands. The Creator made its grooms on two heads:
  `SKM_Groom_Head_Legacy02` is exactly where Kellan's head is; strands made on `Legacy01` (lower and smaller) are moved onto it
  by the scale and offset that fit Legacy01 onto Legacy02 vertex by vertex (1.10). Each copy and each of Kellan's own grooms
  (`GB_Kellan_*`, for his eyelashes and for pawns from older saves) gets a binding to the shaped face (`GB_*`).
- **Marks (V3).** Our own masks drawn in code (256 px, greyscale): `T_Tattoo_0..9` in `tattoos.styles` order (tribal spikes,
  stripes, anchor, rose, sun, skull, chain, wings, dragon, knot), `T_Scar_0..5` in `scars.styles` order (slash, three claw
  cuts, a mottled burn, a puncture, a ragged tear, a stitched line) and `T_BodyHair` (short strokes). `M_PawnMark` is a
  translucent decal of one colour times a mask and an opacity, gated to where custom depth equals the scene depth within
  2 mm: the pawn's skin (MetaHuman body, face and the bare skin layer) draws into custom depth while it has marks, so a mark
  shows on skin and never on the clothes over it (nor anywhere if custom depth is missing).

In the game (`ApplyLooks`, MetaHumans only):

- The face mesh is swapped for the shaped copy, its morphs set from the sliders and gender, the eyes given their colour's
  instance, and Kellan's grooms rebound to it. A pawn from an older save (no hair style) keeps Kellan's face shape, eyes and grooms.
- Hair, beard, moustache and brows: `SetGroom` puts the style's groom and binding on Kellan's groom components (`Hair`,
  `Beard`, `Mustache`, `Eyebrows`); none for Bald, None, or a style with no groom. The brow slider picks among the gender's
  brows. Women never get a beard (the sim gives them None). Hair colour goes on as before (melanin, redness). A groom with
  nothing to draw is hidden; grooms show only at full detail, and hair under a covering hat stays hidden.
- Marks: only at full detail (the nearest 20 pawns; remade when the detail or looks change). One decal per tattoo and scar,
  attached to a bone of the body: a region is a chain of bones (torso spine_01 to spine_05, arms upper arm to hand, legs thigh
  to foot, neck, and the front half of the face) with a radius along it; `v` runs along the chain, `u` once around it (0 the
  front, the face from ear to ear), and the decal projects inward from that point on the body's surface. Tattoos are dark
  ink (4 cm x size), scars paler and pinker than the skin (3 cm x size), body hair four decals in the hair colour on forearms
  and shins (men also chest and belly) as dense as the amount. Decals fade out below 0.4% of the screen.

Measured (`faces`, editor build, offscreen 888x500): twelve pawns in a row all look different (7 hair styles, 6 eye
colours, no two faces closer than 0.18 on the four sliders); 100 MetaHumans with faces and hair, 19.7 ms game thread, 8.0 ms
GPU at 888x500 (the `looks` crowd on the same build: 18.2 ms and 2.4 ms, a smaller scene). Close-ups and portraits at 1080p.

Not done: far pawns have no hair at all (as before; the plan's hair cards for them are still to do), hair does not follow
the face morphs where they meet (the morphs stay off the scalp), the face texture is still Kellan's (male, for women too;
the presets' own synthesized textures could be used later), and eyebrows and beards do not move with expressions. The bare
skin layer (U16) is not the same colour as the face and hands; marks on it show that more.
## In Unreal

`USimPawnSubsystem::MakeLooks` turns the snapshot's looks and the lineage def into an
`FSimPawnLooks` (linear colours, cm); `ASimPawnActor::ApplyLooks` applies it when it changes:

- Height: the mannequin component's scale; the MetaHuman and horns are attached to it.
- MetaHumans only: the meshes are swapped for the `SK_Looks_*` copies and the morph weights set
  (men: `Thin` or `Heavy` by weight; women: `Female` blending into `FemaleThin` or `FemaleHeavy`),
  skin and clothes materials swapped for ours (the face's dynamic instances by their parent), skin
  colour into custom primitive data, hair melanin and redness on the grooms.
- Horns: two `SM_PawnHorn` components on the `head` bone of the visible body, placed from the top
  of Kellan's forehead in his reference pose.

Without the generated assets everything still runs: pawns keep Kellan's look and the log says to
run the commandlet.

## Smoke test `looks` (about 40 s)

The Crashlanded colonists plus 14 pawns, as MetaHumans, every fourth Emberkin, paused. Every frame
it checks each pawn's actor against its sim looks: scale, the four shaped meshes and their morph
weights, materials marked for morph targets, our skin material with the right colour on body and
face, two horns exactly on the Emberkin. Phases: close up (and the looks vary: both genders, three
or more body types, heights and weights spread, the Emberkin reddest), the overseer camera at its
default distance and three notches closer (horn and head sizes on screen in the notes), lineages
changed back and forth, and 100 walking pawns for the cost.

Measured (editor build, offscreen 888x500): 100 shaped MetaHumans 18.2 ms game thread, 2.4 ms GPU,
against 17.1 ms and 2.5 ms for 100 unshaped ones in the `pawns` test on the same build (a different
scene, and this test also runs its own checks every frame). The morphs cost the graphics card
next to nothing. At the default overseer distance horns are about 2 px long and heads
2.6 px tall at 888 px wide (about 6 and 7.5 px at 1440p); three notches closer, 3.6 and 4.4 px.

## Not done here

- **Faces.** V2 draws them: face morphs from the Creator's presets, above. Whole new MetaHuman faces (assembled
  characters) would need the cloud rig and texture download (an Epic sign-in).
- **Proportions.** Shorter limbs, narrower shoulders and wider hips are skeleton, which the leader
  pose takes from the mannequin; body types change shape and the scale changes height.
- **Clothes.** U16 draws the sim's worn apparel over a skin layer and hides Kellan's own top and trousers
  ([clothes-draw.md](clothes-draw.md); meshes: A5, below). Without the clothes assets Kellan's outfit stays.
- **Mannequins** (the default look) get height and horns only.
- **Hair styles and beards**: V2, above (Kellan's stay for pawns from older saves); their colour follows `hair`.

## Apparel meshes (A5)

`python tools/clothes_art.py` (hat download, sets `visual.worn` on each apparel def), then the
`ClothesArt` commandlet (`Rim3DEditor/ClothesArtCommandlet.*`, about 1 minute, PawnLooks first)
makes `/Game/Rim3D/Generated/Clothes` from `assets/clothes_art.json`. Nothing needs a sign-in.

- **Body garments** (`SK_<def>_<n>`, one per part): copies of the morphed hoodie (tops, coats) and
  cargo trousers (legs) with the triangles on named bones cut away (sleeves, hood, shins), thickened
  along the normals (OnSkin 0.2-0.3 cm, jacket 1.1, flak 1.2-1.3, parka 2.4, duster 1.6/2.6) and
  put on a flat `M_Garment` colour (`Color` vector parameter, so U16 can tint by stuff). They share
  Kellan's skeleton and body shape morphs, so they fit every body type like his clothes do.
- **Head pieces** (`SM_<def>`, `"bone": "head"`): cowboy hat from Poly Haven's Fishermans Hat (CC0),
  tuque and simple helmet made in code. Vertices are already in head bone space: attach at the
  `head` bone with no offset.
- Thicker outward by layer (OnSkin < Middle < Shell), so U16 draws in layer order without z-fighting.

U16 also adds `SK_Body_Bare_0` and `_1` (the skin under the clothes, `Body_Bare` in `clothes_art.json`) and gives `M_Garment`
a `Wear` input that cuts holes, so rerun the commandlet after pulling it.

Not done: the duster has no long skirt (thickened trousers instead), no boots or gloves defs yet,
tribalwear is a tunic and shorts from the same two meshes, and none of it has been looked at in the
game (fit, hat position). U16's smoke test should screenshot each piece on thin, heavy and female pawns.
