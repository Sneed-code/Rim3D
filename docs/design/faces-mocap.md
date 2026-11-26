# Live faces from facial motion capture

sneed (2026-11-26): "add all the motion capture facial expressions i want realism". Cosmetic, Unreal side only
(`PawnFaceMocap.*`, `ASimPawnActor::UpdateLiveFace`).

## The capture

The BEAT dataset (Liu et al. 2022, "BEAT: A Large-Scale Semantic and Emotional Multi-Modal Dataset for Conversational
Gestures Synthesis"; CC BY-NC 4.0: free, credit, non-commercial, so fine for this private game): actors telling stories
in eight emotions (neutral, happiness, anger, sadness, contempt, surprise, fear, disgust), their faces recorded with an
iPhone's face tracking as Apple ARKit's blendshape curves at 60 fps. We took two women and two men, four takes of each
emotion (about two hours; the raw files stay on sneed's PC in `Rimworld3D\facemocap\beat`, not in Git).
`tools/face_mocap.py` turns them into `assets/face_mocap/clips.json`: resampled to 30 fps, lightly smoothed, at most
40 s from a take and 150 s of each emotion, taken round-robin from the four people, cut into 6 to 14 s clips at quiet
moments (eyes open, mouth nearly shut), each curve a byte a frame. BEAT has 51 curves (no tongue) and no head motion.

## On the face

`assets/face_mocap/arkit_map.json` says how much of each MakeHuman morph each ARKit curve drives: MakeHuman's 34
expression units (`Face_<Unit>`), left and right halves of the mouth units (`Face_<Unit>L/R`: `creator.json` one-sided
morphs, `mh_common.add_morphs` keeps a unit's change on one side of the middle line with a 1.5 cm blend, so a lopsided
smile or sneer comes through) and `Face_CheekPuff`. Eye look, jaw sideways and tongue drive nothing yet (the eyes have no
bones of their own). Tuning a mapping needs no build: the file is read when the game starts.

Each pawn within `Pawns.LiveFaceMetres` (15 m) of the camera plays clips of the emotion the sim gives it, a new take of
the same emotion when one ends (from a random point the first time, so two people never move together), eased (80 ms,
0.35 s just after a change). The emotion, strongest first:
- dead: slack, lids half down, jaw a little open (no capture);
- asleep or knocked out: eyes shut;
- a mental break, a fist fight or melee (the angry fight face): anger;
- awake while downed, the pain face, or much pain: fear (talking while downed: moaning and begging);
- talking (an interaction showing): happiness when its mood is high, sadness when low, contempt for an insult (a failed
  talk), else neutral, mouth in full;
- mood under 25 or a break risk band of 2+: sadness; over 75: happiness (softer); else neutral.

While not talking, the mouth curves are turned down (`quiet` in the map; the actors were talking) so a quiet face
blinks, glances, frowns or half-smiles but doesn't mouth words. The fight faces (`PawnExpressions.h`) go on top, the
capture under them turned down as they come in. The skin's wrinkle maps follow the live face. `Pawns.LiveFace 0`
turns it off. Faces further away stay still (cheap).

The Debug creator drives its own faces (expressions, blink, talk) unless its Face tab's "Facial capture" emotion is set.

## Next

- Lip sync from speech audio when pawns get voices (BEAT has the takes' audio; NVIDIA Audio2Face makes ARKit curves from
  any audio).
- Eye bones for the eye look curves, head nods from the capture's timing.
- More takes and people from BEAT (30 speakers) if the variety shows repeats.
