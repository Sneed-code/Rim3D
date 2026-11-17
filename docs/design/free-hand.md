# Free hand

sneed (2026-11-17): a free hand mode for the player's own body. The hand moves with the mouse and touches the world;
clicking grips. Not made for any one thing: the hand never wraps its fingers round a face or a cup. It goes where the
marker says and the world reacts with the physics already there (soft skin, ragdolls, loose pieces). Works on every
body and everything in reach. Pure looks: nothing here reaches the sim.

Code: `game/Source/Rim3D/FreeHandSubsystem.*`; the arm and fingers in `PawnAnimInstance.cpp` (`FSimPawnHandNode`) and the
MakeHuman body's own wrist reach (`UMakeHumanAnimInstance::SetRightHandReach`); the palm presser in
`SoftContactSubsystem.*`; the head hidden in first person in `PawnActor.cpp` (`SetHeadHidden`).

## Keys (direct control)

| Key | What it does |
|---|---|
| J | free hand on or off (H is taken: it patches wounds) (the weapon goes to its holster, the mouse buttons stop shooting, swinging and blocking) |
| mouse | moves the hand's marker (a ring on screen); pushed past the screen's edge, the view turns |
| left mouse, held | grip; let go to release (a carried piece keeps the hand's speed: a toss) |
| wheel | how far the hand reaches into the air (35 % to all of the arm) |
| right mouse, held | look around; the marker stays where it is on screen |

The ring is white in the air, green resting on something, gold while holding. Free hand goes off by itself when control
ends, the colonist goes down, or a building is picked to place. Walking keeps working.

## How it works

- **The marker** is a point on the screen. A ray from the camera through it finds the first thing within arm's length of
  the right shoulder: a body (capsule stand-ins for limbs, trunk and head, a little fuller than the soft-contact cores;
  `Hand.FleshPad` adds to them all) or anything with collision (walls, ground with collision, ragdolls, debris).
  Nothing in reach: the hand goes to where the ray leaves a ball round the shoulder (the wheel sets the ball's size).
- **The arm** (`FSimPawnHandNode` on the mannequin, so a MetaHuman follows through the leader pose): a two-bone solve on
  upperarm_r, lowerarm_r and hand_r, the elbow bent out, down and back; the palm turned to face what it rests on (its way
  found from the knuckles and fingers, so no bone axis is assumed); the fingers curled towards the palm (a little in the
  air, resting on the skin while holding, most when carrying). A MakeHuman body takes the arm and fingers through its
  retargeting and then its own wrist reach puts the hand exactly on the spot (its arm is not the mannequin's length).
  Standing, the body turns to face where the player looks.
- **Skin**: the palm is a presser capsule in the soft-contact table, so the skin squishes round it (MakeHuman bodies; the
  squish runs while free hand is on even if `Pawns.SoftContact` is 0). A touch landing wobbles the soft parts near it as
  hard as the hand came in, and moving over the skin drags the flesh a little (`Hand.Kick` scales both). Gripping a body
  keeps the hand on that spot as the body moves (it rides on the nearest bone) and presses in `Hand.Press` (2 cm).
- **Loose things**: a small kinematic ball at the palm shoves physics bodies it runs into. Gripping one that weighs up to
  `Hand.MaxGrabKg` (30 kg) carries it on a physics handle (a ragdoll by its limb, debris); heavier ones are only pushed.
- **First person**: the body is drawn with its head (and face, hair, hat, horns) hidden, so the arm and chest show. In the
  gun range the first-person gun arms hide meanwhile.

## Tuning

`Hand.MarkerSpeed` (marker speed, times the mouse turn speed), `Hand.FleshPad`, `Hand.Press`, `Hand.MaxGrabKg`,
`Hand.Kick`, `Hand.ShowShapes 1` (draws the body stand-ins).

## Trying it

`tools\play-gunrange.bat`: a partner woman stands at arm's length ahead and to one side. Walk up to her, press J.
`-FreeHandScript` (smoke test `gunrange`) runs a short tour instead of the gun tour: walks up, rests the palm on her cheek
and holds it, then her belly, the same in first person, then hands back; pictures in `game/Saved/GunRange/hand_*.png`.

## Not yet

Only the right hand. No finger contact of their own (the curl is the same for anything held). Bodies drawn as MetaHumans
have no soft skin to squish. Items lying in the sim's world have no physics: picking those up is F as before.
