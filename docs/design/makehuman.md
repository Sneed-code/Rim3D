# MakeHuman characters (MPFB)

A second way to make people, next to the MetaHuman route ([looks.md](looks.md)), after the review of the real
MetaHuman body: MakeHuman's one shared base body (both sexes from one mesh, anatomy proxies, consistent nude skins),
built with MPFB in Blender and retargeted to the game's animations. This first step proves the pipeline end to end
with one example woman and a comparison scene; the live in-game creator (sliders as morphs, joints that follow
proportions, clothes that follow) is the next step.

Code: `tools/makehuman/mh_export.py` (headless Blender + MPFB), `tools/makehuman/woman.json` (the example),
`assets/makehuman.json` (what to import), Unreal `Rim3DEditor/MakeHumanArtCommandlet.*` (import),
`Rim3D/MakeHumanArt.h` (names), `Rim3D/MakeHumanAnimInstance.*` (retargeting), `PawnActor.cpp` (`ApplyMakeHuman`),
smoke test `compare`.

## Making a character

1. `blender.exe -b -noaudio --python tools/makehuman/mh_export.py -- tools/makehuman/woman.json` builds the human
   with MPFB (macro sliders: gender, age, muscle, weight, height, proportions, cup size, firmness, ethnicity), adds the
   `game_engine_with_breast` skeleton (the UE4 mannequin's bone names plus breast bones for jiggle later), the body
   proxy (the AGPL3 anatomy bodies: check the licence before shipping), the skin and any clothes (pubic hair), bakes
   it, and writes `game/Saved/MakeHuman/<Name>/<Name>.fbx` with the skin's colour map beside it as `skin.png`. The
   export is in centimetres with an unscaled, unrotated root (Unreal's retargeter measures bone heights). `{mh_user}`
   and `{mh_system}` in a config are MakeHuman's data folders (`MH_USER_DATA`, `MH_SYSTEM_DATA`). About 5 seconds.
2. `UnrealEditor-Cmd.exe game\Rim3D.uproject -run=MakeHumanArt -unattended -nosplash -nullrhi` imports every
   character in `assets/makehuman.json` into `/Game/Rim3D/Generated/MakeHuman` (not in Git; it starts clean):
   - the skeletal mesh, its skeleton and physics asset;
   - its skin on our skin material (`MI_MH_<Name>_Skin` on `M_PawnSkin`, so the pawn's skin colour drives it);
   - an IK rig (a copy of the UE4 mannequin's: same bone names) and a retargeter from the game's (UEFN) mannequin, a
     copy of the Game Animation Sample's UE4 one with every op pointed at the new rig, the two UE4-tuned leg ops
     ("Blend to Source", "Offset Goals", which planted a smaller body's feet where the mannequin's are) off, and the
     target retarget pose turned so each chain bone points where the mannequin's does.
   Run PawnLooks first (the skin material). Models that bring their own textures list `materials` (slot -> colour,
   normal, opacity, skin) and, for other skeletons, `chains` and `retargetRoot`.

## In the game

`FSimPawnLooks::MakeHuman` (a character's name) draws that character instead of the MetaHuman: a skeletal mesh
attached to the hidden mannequin, animated by `UMakeHumanAnimInstance` (one `RetargetPoseFromMesh` node with the
character's retargeter; the sample's generic retarget animation picked retargeters from its own list and ignored
ours). Its mesh has its own height, so `Height` is 1 for it. For now only a looks override sets it (smoke tests).

## Jiggle and hair

`UMakeHumanAnimInstance` runs, after the retarget, one AnimDynamics soft body on each breast bone (MPFB's
`game_engine_with_breast` skeleton: it lags behind the body's motion within about 1.5 cm and 8 degrees and springs back)
and a swinging chain on a ponytail's bones (MakeHuman's ponytail sub-rig, joined into the main skeleton under the head
by `mh_export.py`; enable Blender's Rigify add-on, which MPFB needs to read older sub-rigs). Simulated in world space,
so walking and turning move them. `Pawns.MakeHumanPhysics 0` turns it off (read when a character's animation starts).
Hair without bones (the long01 style) stays still. There are no butt bones in this skeleton yet.

sneed's Petite Natural Pale Adult is rebuilt from his MakeHuman save (`tools/makehuman/petite_pale.json`, `"mhm"`) with
this skeleton rather than exported from his finished .blend (`blend_export.py`, Mixamo skeleton, no breast bones).

## Showcase (play)

`tools\play-showcase.bat` starts the editor build on the compare scene for play (smoke test `showcase`, which checks
nothing): the Crashlanded start on a cleared flat map, its first two colonists drawn as the ponytail woman (the player's:
direct control) and the long-haired example woman (drafted, standing), the rest removed, graphics turned up (epic
scalability, full resolution, TSR, Lumen GI and reflections, virtual shadows, contact shadows). `-ShowcasePlayer=` and
`-ShowcaseNpc=` pick other characters. Hardware ray tracing is off in the project (Lumen traces in software).

## Smoke test `compare`

Three pawns side by side on a cleared flat map in the morning sun: the MakeHuman example woman, a local-only
comparison model (`-CompareLocal=<name>`, default Shadowheart), and the MetaHuman showcase woman with a ponytail.
Standing, close up, each face and body, then drafted and walking back and forth (24 frames for a clip:
`ffmpeg -framerate 5 -i compare_walk_%02d.png ...`). Every frame it checks each MakeHuman character shows its own
retargeted mesh and the MetaHuman is hidden. Measured: the retargeted thigh and upper arm point within 1 degree of the
mannequin's.

**Local-only comparison models** (other people's characters, such as a fan model of a game character) are listed in
`assets/makehuman.local.json`, which is not in Git, and import into `/Game/ThirdParty/Local`, which is not in Git
either. They are never committed or shipped.

## Not done

- Hair, eyes, eyebrows, eyelashes and teeth for the MakeHuman woman: MPFB needs MakeHuman's system asset pack (the
  MakeHuman install has them only pre-compiled).
- The live creator: every macro and detail target as a morph on one base mesh per sex, the macro blend in game, the
  skeleton's joints moved per character, clothes fitted once with the same morphs, anatomy and hair as parts.
- Wiring MakeHuman characters into the sim's looks (only overrides use them), clothes on them, LODs.
