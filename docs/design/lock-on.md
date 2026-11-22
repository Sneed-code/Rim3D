# Lock-on, Elden Ring style

sneed, 2026-11-18 (thread "Rebuild lock-on like Elden Ring"), after playing the combat range. Replaces the player-facing
parts of [focus.md](focus.md) (Tab/F1 lock-on, facing, the player's swing); the sim's `FocusCommand`, the fighter AI's
focus, footwork and the HP bar stay. Built on "Make melee move freely and staggers keep legs" (#361) and the range follow-up
(#363, middle mouse toggles the lock); merged after them.

Elden Ring was not watched on the PC: the design is from how its lock-on is known to play. All numbers below are ours
(FromSoftware publishes none) and go in `data/defs/Rules/MeleeRules.json`.

## What sneed asked for

- Lock-on that plays like Elden Ring, for the player **and** every pawn.
- **Melee in hand**: the body faces the target, WASD walks round it, and every swing, stab or punch goes to the target's
  **focus point** so melee is easy to land. It only lands if you are actually in reach, and the damage is whatever the
  blow physically hits on the way.
- Focus point: a humanoid's **torso** centre; for **fists**, a humanoid's **head**; for a big creature, the centre of
  its **body part nearest you**.
- **Gun in hand**: the lock stays (marker, HP bar) but the body does not turn to it and the aim is completely free.
- Switching between melee and gun keeps the lock.
- **Turning away and moving away** from the target lets go.
- Not on **F1**: in the editor build F1 is Unreal's wireframe view (the "polygons"), and other F keys are view modes too.
- Later: spells and aimed abilities use the same lock to aim.

## How Elden Ring plays (what we copy)

1. One button toggles the lock (middle mouse / right stick press); the lock picks the enemy nearest the centre of the
   view. Flicking the mouse or stick left or right switches to the next enemy on that side.
2. Locked on, the character walks facing the enemy: forward closes in, back steps away still facing it, left and right
   circle round it. The camera sits behind you looking past you at the enemy.
3. **Sprinting** turns you to run where you press, so you can run off; a roll goes the way you press.
4. An attack **turns toward the enemy during its wind-up** (fast at first, then locked once the blade moves), with a short
   lunge built into the swing. It doesn't teleport you or bend the swing: if the enemy is out of reach the blow whiffs.
5. Large enemies have several lock points (head, legs, body); the lock can move between them.
6. The lock drops when the enemy dies, gets far away, or is out of sight for a few seconds.

## Ours

### Controls

| Key (rebindable) | What |
| --- | --- |
| **Middle mouse** | lock on to the enemy nearest the centre of the view; press again to let go |
| **Tab** | switch to the next enemy (as now) |
| **Mouse flick** while locked with a melee weapon | a quick sideways flick switches to the nearest enemy on that side |
| F1 | nothing by default (editor build: wireframe) |

`EndFocus` becomes `LockOnToggle` (default `MiddleMouseButton`); `FocusNextTarget` stays on Tab. Old saved key files
that had EndFocus on F1 move to the new default.

### Facing and movement

- **Melee weapon or fists in hand**: the sim turns the body to the target (as `MeleeSystem::faceFocus` does now). WASD is
  read **around the target**: W toward it, S straight back, A/D walk a circle round it at the current distance (the move
  direction is the tangent, corrected each step so the distance holds). The third-person camera turns to keep the target
  ahead (as `FollowFocus` does now); the mouse tilts it and flicks switch targets. The turn is eased, not a fixed
  spin: it closes its gap with a 0.08 s time constant, capped at 360 degrees a second (100 in first person). After a
  roll the cap starts at a tenth and eases up to full over 0.5 s, so the view swings back to the enemy gently instead of
  whipping round (sneed 2026-11-19).
- **Gun in hand**: no auto-face, no camera pull. WASD and the mouse work exactly as unlocked. The marker and HP bar stay.
- **Sprint while locked**: the body turns to the run direction (as Elden Ring), facing returns when sprint stops.
- **Roll**: goes the way you press, relative to the target when locked with melee.

### Letting go

The lock drops when any of these hold:

- the target dies, leaves, changes level, or is beyond `focusBreakRange` (25 m, as now);
- it has been out of sight for `lockSightLostSeconds` (3 s);
- **turned and moving away** (sneed's rule): for `lockTurnAwaySeconds` (0.75 s) you are moving away from it (more than
  `lockTurnAwayMetresPerSecond` 1.5 m/s of your speed is away from it) **and** your body or camera faces more than 100
  degrees off it. With melee that happens when you sprint away; with a gun when you turn the view and walk off.
  Backing away while still facing it (S with melee) keeps the lock;
- Middle mouse again.

Switching weapon never drops it.

### Melee homing (the big one)

When a swing, stab or punch starts while locked on (player or pawn):

1. **Focus point** (`MeleeSystem::focusPoint`): humanoid torso centre (the chest hitbox); with fists, the head hitbox;
   for a creature of body size over `lockBigBodySize` (2.0, a bear and up), the centre of its hitbox nearest the
   attacker, picked fresh at each swing. Animals without hitboxes yet: their body centre as now.
2. **Wind-up tracking**: during the wind-up the body follows the target, and the strike's line is fixed `commitSteps`
   (0.2 s) before the blow (as built for real-time combat), so a dodge at the last moment still works.
3. **Aim**: the swing's line goes at the focus point's height (no camera aim needed). Spear and knife thrusts go straight
   at it; cuts and smashes pass through it in their arc.
4. **Short lunge**: the swing's own step forward already counts in its reach (`engageDistance` adds the attack clips'
   `lunge`, and the player's free-feet swing reaches that far, #361). Beyond that the blow whiffs. No extra number.
5. **The hit stays physical**: the strike is the existing swept line and arc against hitboxes (`MeleeSystem::strike`).
   Homing only decides where it is aimed. An arm or shield in the way takes the blow; a head that ducks is missed.

Without a lock, swings go where the camera looks, as now.

### Pawns

sneed (2026-11-18, after approving the plan): the AI's lock-on is mainly for melee fighters; every other AI rule stays
(fleeing, cover when shot at, weapon swaps by range). Added with it:

- **The nearer threat first**: a fighter without a gun counts an enemy swinging at it, or within reach of it while going
  for it, `meleeThreatMetres` (4 m) nearer when it picks its target (`FighterSystem::threatensInMelee`), and a new enemy
  winding up at it, nearer than its target, makes it think again at once rather than at its next plan
  (`attackedByOther`). It doesn't let one man cut it down while it goes for another further off.
- **Gunmen backpedal**: an enemy without a gun within `backpedalRange` (5 m) that it knows of: between shots the gunman
  steps back `backpedalMetres` (0.8 m in 0.5 s) away from it through the body motion system, and fires at it from where
  it stands as soon as its gun is ready (`FighterSystem::backpedal`). Closer than `switchMeleeRange` (2.5 m) it takes up a
  melee weapon if it carries one, as before.
- Smarter squad play (pinning and flanking, call-outs) is a later thread.

The fighter AI already locks a melee fighter on within `aiFocusRange` (8 m). Pawns use the same homing (1 to 5) and the
same movement rules: melee pawns circle with footwork facing the target, gun pawns keep the lock without being turned
by it (they face their aim). The AI's wobble by skill (`aimWobbleBySkill`) stays so skill still matters.

### Spells and abilities (later)

An aimed ability asks `focusPoint(target)` for its aim when the caster is locked on, else the camera's aim. Guns never do
(sneed: free aim).

## As built (2026-11-18)

- Sim: `MeleeSystem::focusPoint` (used by every swing at a standing target, player or AI), `orbitMove` (DirectMove while
  locked with melee: a step whose part toward or away from the target is under `orbitRadialShare` (35%) of it is put back
  on the circle), `faceFocus` skips a pawn with a gun in hand and lets the player's lock go after `lockSightLostSteps` (90)
  without line of sight (melee save section 5: `focusSeenAt`). Tests: `LockOnTests.cpp`.
- Unreal (not compiled yet): `UProtagonistSubsystem::UpdateTurnAway` (turned away and moving away at over 1.5 m/s for
  0.75 s lets go: with a gun the view more than 100 degrees off, with melee sprinting), `AProtagonistPawn::FollowFocus`
  doesn't pull the view with a gun in hand or while sprinting.
- Not yet: the mouse-flick target switch, the marker moving to the head with fists, nearest-to-view-centre first pick
  (Tab's nearest-in-front pick is used), and the body turning to run while sprinting locked on (the lock lets go after
  0.75 s instead).

## Tab, flicks and landing blows (sneed 2026-11-19, after the gun range)

sneed: "tab doesn't shuffle or work intuitively to target closest", "when I'm swinging at someone it doesn't always hit,
and they don't dodge it or anything", swing speeds up (club x2, the rest +50%), a spear or knife at a downed target stabs
down into the body.

- **Why Tab felt stuck**: it took the one after the held target in a list sorted by distance at each press. Walking to
  the second nearest made it the nearest, so Tab swapped between the two nearest for ever. **Now a round**: each Tab
  within `focusRoundSteps` (5 s) of the last gives the nearest enemy not given yet (standing ones before downed ones,
  those in view before those behind), then round again from the nearest. After 5 s without Tab a new round starts
  from the nearest other than the one held. "In view" is where the **camera** looks (FocusCommand `viewX/viewY`), not
  where the body faces.
- **Mouse flick** (Unreal `UProtagonistSubsystem::UpdateFlick`): locked on with melee or fists, a quick sideways mouse
  move (`LockOn.FlickMouse`, 40 units within 0.15 s, 0.4 s between flicks) sends FocusMode `Left`/`Right`: the enemy
  with the smallest turn that way from the one held (within `flickMaxDegrees` 150). Not with a gun (free aim).
- **The practice range** picks its own-side targets through FocusCommand `anySide` (it used to pick them itself, Tab
  only, so middle mouse found nobody there).
- **Faster swings**: `data/tuning.json` cooldown multipliers (RimWorld's numbers stay in the defs): club 0.5, knife
  0.222 (x3 then +50%), longsword, breach axe, spear and monosword 0.333 (x2 then +50%), gladius, mace, plasma sword and
  zeushammer 0.667. The wind-up is `windupShare` of it (at least `minWindup`, 0.3 s) and the clip plays to fit.
- **Why swings whiffed**: (1) a swing started with the lock just out of reach went straight ahead at nobody; (2) the
  line fixed 0.2 s before the blow missed a target that only shuffled (footwork) without dodging. Now the player's swing
  takes its lock up to `playerReachSlack` (0.6 m) past its reach (it walks on under it; the blow is still only as long as
  the reach), else the nearest enemy within `assistCos` of ahead, else whoever walked into it by the commit. A target
  that didn't dodge is followed to the blow if it moved no more than `strikeTrackDegrees` (35) off the line.
- **Why nobody dodged**: the faster swings wind up in 9 steps; reading one took 14 steps at skill 0 and 5 at skill 20
  (plus or minus 4), so most were too quick to read. Reaction is now `reactionStepsBySkill` 9 to 3 (plus or minus 3),
  and a swing read in time is defended `defendChanceBySkill` (35% at skill 0, 85% at 20): with a weapon a parry
  (`parryShare` 60% for a fighter), else (and the rest) a roll from a heavy blow, a **step back** out of reach
  (`backstepShare` 40%, 1.3 m in 8 steps, clear of the attacker's own step in) or a sidestep. A dodge read in time keeps the fixed line, so it gets away.
- **Blows at the downed**: the sim already picked the downward stab (`Motion_Spear_Stab_Low`, `Motion_Knife_Stab_Low`)
  for a swing at a downed target; the PC's range build lacked those clips and played the standing one. In first person
  the hands now drive down into the body for a low blow instead of straight ahead.

## Finishers, aim assist and Tab that keeps you running (sneed 2026-11-20)

sneed: "melee attack with spear / other melee weapons doesn't change on a downed enemy ... they have to be unique (a spear
rams it in the back, club smashes, sword thrusts into chest, knife stabs down like slamming a knife into the table) - this
requires my pawn to kneel down"; "if I use a melee weapon near a non-friendly (even neutral) it should have aim assist";
"when I use Tab do not let it interrupt my movement (or sprint)"; "pressing Tab again still doesn't change targets".

**Finishers (sim).** A swing at a body on the ground with a weapon that has a finisher is a finisher, not a swing:
`MeleeRules.finishers` names one per weapon (`Melee_Spear`, `Melee_Knife`) or per style (`Club`, `OneHand`, `TwoHand`),
each with its move (Spear, Club, Sword, Knife), total `seconds`, `hitSeconds` (when the blow lands), `standOff` (how far
from the body's centre the attacker stands or kneels), `kneel` and `bend` (how far the back bends). The attacker steps to
its place beside the body before the blow (`finisherStepMetres` a step, through BodyMotion), holds still (DirectMove is
ignored, facing stays on the body) and is ready again when the move ends. Melee save section 7 keeps a finisher under way.
The snapshot carries `meleeFinisher` (1-based index), `meleeFinisherT` (0..1) and `meleeFinisherTarget`.

| Weapon | Move | Look |
|---|---|---|
| Spear | Spear | stands over the back, both hands raise the spear high, rams it down into the lower back |
| Club, mace | Club | drops to one knee, swings the club up and smashes it down on the nearest part of the body |
| Gladius (one hand) | Sword | kneels, reverse grip, drives the blade down into the chest |
| Longsword (two hands) | Sword | stands, both hands, point down into the chest |
| Knife | Knife | kneels, reverse grip, raises the knife and slams it straight down into the chest like into a table |

**Drawing (Unreal).** `UWeaponDrawSubsystem::FinisherPose` moves the weapon along the move's keys (points in front of,
right of and above the target's bone, `spine_02` for the spear, `head` for the club, `spine_03` for swords and knives;
Hermite curves that hold still at held keys, no snapping) so the tip ends on the body at the hit; the hands are pinned on
the weapon (weapon first, then the hands, as `weapon-hold.md`). It eases in over the first 14% and out over the last 14%.
Kneeling moves play the crouch pose on the default slot while T < 0.86; the back bends by `bend` with an extra push near
the hit. The spot is whichever of the move's bone (15 cm preferred), the spine, pelvis and thighs is nearest the
attacker's shoulder (a ragdoll lies any way round). Where the grip would still be beyond the arm's reach (0.97 of the
arm from `upperarm_r`), the weapon comes back along that line so it never leaves the hands, and the back bends up to 30
degrees further while the spot is still ahead of the shoulder. The bend is put on after the montage slots (the kneel is
a whole-body clip there). The drawn body never slides toward the victim. A finisher whose body leaves the map ends at
once. Only MakeHuman bodies (the gun range, the creator) pin the hands to the finisher path; MetaHumans get the sim part
(step in, hold, kneel, bend). Smoke test `finishers` pictures each move at the raise, strike and after (`Saved/Smoke/finishers/`).

**Aim assist on anyone not friendly.** With no lock-on, the player's swing goes at the best target in a 70 degree cone
ahead within reach: hostiles, **neutrals too** (another faction, wild animals), never its own side or prisoners (sim
`MeleeSystem::setNotFriendly`; Unreal `FindSwingTarget` the same). The most centred wins; the nearest when two are within
a few degrees. The body also keeps facing its target through the windup now: the walk direction no longer turns it away
(the sim's combat step and the pawn actor both used to face the walk, which made swings "wonky").

**Tab no longer stops you, and every Tab cycles.** The cause was Unreal's menu navigation: Tab (and the arrow keys) moved
keyboard focus off the game viewport to the next widget, so held keys were let go (W and sprint stopped) and the next
Tab went to that widget instead of the game. `UProtagonistSubsystem::Initialize` turns Slate's Tab, arrow and stick
navigation off for the game (and puts it back on shutdown). A flick with nobody that way (one other target left, behind or on the other side) still switches, to the other
target nearest in turn (PC gun range 2026-11-20). Smoke `finishers` phase `tab_run` sprints with real key
events, presses Tab three times and checks the speed never drops and the lock moves to three different enemies.

## Whole-body finishers, held blocks, no crouch slides (sneed 2026-11-22, after the gun range)

sneed: the longsword's downward stab "only moved the arms": the hand-made finisher pose (an arm path, a crouch laid on
the legs, a waist bend of up to 60 degrees) pushed the arms through the attacker's own body, and the knife's version
shook and showed the joints at the waist. **Each finisher now plays a whole-body mocap clip** (`MeleeRules.finishers.*.clip`,
a MotionClips def): the knife kneels and drives both hands down (Mixamo Farming "Plant A Plant", 1.6-3.3 s, blow at
2.4 s: `Motion_Knife_Kneel`), the gladius and mace hack low (Sword And Shield Slash), the club smashes overhead
(Melee Axe Downward), the longsword hacks low two-handed (Great Sword Slash), the spear drives down two-handed (the
longsword's low hack: the Fighting spear stab thrust level, over the body, in the PC pictures). The
sim plays the clip on the whole body, timed so the blow lands at `hitSeconds`, and the finisher lasts as long as the clip
(`MeleeSystem::finisherPlaysClip`). Unreal draws no hand-made pose, kneel or bend for it: the weapon stays in the hand
the clip moves, like any clip swing. Unreal sets the drawn hips back to the body's place
through a clip, so the blade reaches only as far as the arms and weapon do from the hips: `standOff` (the distance to
the body's middle at the blow) is knife 0.8 m, club 0.9, gladius, mace and longsword 0.85, spear 1.1 (PC round 2: at
1.25-1.5 the longsword and spear came down short of the body). The attacker steps to it over the first two thirds of
the wind-up (`BodyMotionSystem::shiftClip`, laid over the clip's own path; the world still stops it), so the blow lands
on the body wherever the swing started. More than `clipSlack` too far, the weapon's low blow plays instead; right on top
of the body it steps back at most `clipSlack` and still plays the finisher (round 2: the gladius's never played). The
`finishers` smoke takes a `_blow` picture at the hit and notes where the drawn bodies and the weapon are. The clips'
measured `hitSeconds` are mid-swing (round 3: every weapon still about a metre up at the blow), so each finisher names
`clipHitSeconds`, the moment in its clip the blade comes down (`MeleeSystem::finisherClipHit`), read off the clips' hand
heights. Round 3 also showed the downed body drawn 0.4-1.2 m off its spot (its ragdoll falls back from the feet, while
the sim's prone hitboxes lie centred on the spot); round 4 showed no one way (50-70 cm, any direction), so a downed
body's ragdoll now steers its pelvis back over its sim spot while it falls and after a push wakes it
(`ASimPawnActor::UpdateRagdollTether`: a speed it eases toward, up to 1.6 m/s, for the first 2.5 s; asleep it is left
alone; round 5: steering the pelvis alone could not drag the body against the ground's grip, so the whole body is
steered alike). Dead bodies are not steered. Round 6: spear, club, gladius and longsword all come down on the body. The
knife tried the gladius's standing low hack in round 6, but a knife's short blade stopped 40 cm above the body, so it
kneels again (the kneel had looked like lying on the body only because the body was drawn off its spot), 0.85 m off. Round 4 with the blow timing: spear and gladius hit, club just short, longsword and
knife overlapped the drawn body (it had fallen toward them). A finisher whose clip is not measured yet falls
back to the old drawn one.

The punches and the low blows (`Mixamo/Fighting/*`) had never been imported into the one game (they were made in a test
copy), so those moves had nothing to play. The art step `mixamo-extra` copies their prepared FBX files from
`assets/raw/mixamo/extra/` into the Mixamo manifest and imports them (`tools/makehuman/mixamo_extra.py`).

**Blocking.** Right mouse with a melee weapon or bare fists blocks, for as long as it is held (`MeleeSystem::block` sent
every step keeps it up `playerBlockHoldSteps` longer; the first press still gives the `playerBlockSteps` parry window).
Fists stop a punch; a weapon's blow on raised fists lands on the left arm instead of the body (`armBlocks`). A gun aims.

**No crouch slides.** Every pawn action that had no animation of its own (a block, a dodge, a reload, a weapon switch,
a throw, an alert, a dive...) fell through to the pick-up crouch on the whole body for 0.6 s, so fighters ducked every
time they parried or reloaded and slid along on crouched legs if they moved. Only standing work (pick up, put down,
eat, build, cook...) crouches now, never while moving faster than 40 cm/s, and a crouch stands up at once if the body
starts to move.

## The original build plan

- **Sim (cloud)**: `focusPoint`, wind-up turn rate, lunge, letting go (turn-away and sight timers), "locked but don't
  face" with a gun (`faceFocus` skips a pawn with a gun drawn, not only one aiming), circle-strafe move for the player's
  `MoveCommand` while locked with melee. Doctest tests for each, a scenario test of a locked duel.
- **Unreal (one short PC compile)**: middle-mouse toggle, nearest-to-view-centre pick, mouse-flick switch, F1 freed,
  camera follow only with melee, marker on the focus point (moves to the head with fists). Smoke test `melee` phase
  `focus` extended; checked in the combat range.

It depends on the two fixes queued before it: moving freely while swinging (the root motion change), and staggers with
working legs. Homing rides on the swing motion they rebuild.
