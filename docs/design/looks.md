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

- **Faces.** Every pawn has Kellan's face (women too); only its colour changes. More faces need
  MetaHuman Creator characters assembled, which needs the cloud rig and texture download (an Epic
  sign-in), or face morphs made from its presets.
- **Proportions.** Shorter limbs, narrower shoulders and wider hips are skeleton, which the leader
  pose takes from the mannequin; body types change shape and the scale changes height.
- **Clothes.** U16 draws the sim's worn apparel over a skin layer and hides Kellan's own top and trousers
  ([clothes-draw.md](clothes-draw.md); meshes: A5, below). Without the clothes assets Kellan's outfit stays.
- **Mannequins** (the default look) get height and horns only.
- **Hair styles and beards** stay Kellan's; their colour follows `hair`.

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
