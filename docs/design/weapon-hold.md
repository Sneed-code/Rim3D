# Holding weapons and shields

sneed (2026-11-17): "how hard is it to create a system to make each weapon / shield etc animation work? ... does it
only need to be done once?" Yes: this is that system, built once. Every weapon and shield says how it is held (a hold
style and its grip points), the hands close on those points, and each style has one set of clips. A new weapon adds
only its `hold` block (or nothing: it falls back by its kind).

## Data

`data/defs/HoldStyles/HoldStyles.json`, one def per way of holding something:

| Style | Grip | Hands | Swings (MeleeRules `styleAttacks`) | Clips |
|---|---|---|---|---|
| Hold_Unarmed | None | 0 | Unarmed | block, fpStance (a guard) |
| Hold_OneHandBlade | Blade | 1 | OneHand | stance, block, shieldStance, shieldBlock |
| Hold_Club | Blade | 1 | Club | stance, block |
| Hold_TwoHandBlade | Blade | 2 | TwoHand | stance, block |
| Hold_Spear | Pole | 2 | TwoHand (the spear's own `Melee_Spear` thrusts first) | stance (rifle idle: a low forward guard), block, fpStance |
| Hold_Pistol | Gun | 1 | | stance, aim, reload |
| Hold_Rifle | Gun | 2 | | stance, aim, reload |
| Hold_Bow | Bow (left hand) | 1 | | stance, aim, draw |
| Hold_Shield | Shield (left forearm) | | | block |

A style gives `hand`, `grip`, `hands`, `attackStyle`, default grip points (`main`, `off`, cm), `rake` (a pistol grip's
slope) and clip paths under `/Game/Rim3D/Generated/Animations/`. A first-person key (`fpStance`, `fpBlock`) falls back
to its third-person one.

A weapon def's `hold` block: `style`, `main` (where the holding hand closes, in the weapon's own frame: x along the
blade or barrel), `off` (where the other hand goes on a two-handed weapon), `rake`. All optional. Without a style the
sim picks one by kind (`colony::holdFor`, `Hold.cpp`): a gun is a pistol or rifle by `twoHanded`, a melee weapon a one-
or two-handed blade, nothing is fists. One-handed styles never put the other hand on. Shields use `shieldHoldFor`
(default Hold_Shield). `tools/check_defs.py` checks every style word, point and attack style.

The melee swing a weapon gets comes from its style's `attackStyle` (MeleeRules `weaponStyle` can still override one
weapon); `MeleeSystem::attackClipsOf` lists them.

## Drawing (Unreal)

`HoldDraw` (`game/Source/Rim3D/HoldDraw.h`) is the only code that knows how a hand closes on a weapon, used by every
body (third person, the first-person arms):

- the hand's frame from its bones (`BladeFrame` a fist round a hilt, `PoleFrame` a shaft from the holding fist through
  the other one (spears: whatever the arms do, the shaft lies along both hands, point ahead of the front hand), `GunFrame` a pistol grip raked back,
  `ShieldFrame` the left forearm's strap);
- the weapon placed so its `main` point sits in that frame (`WeaponWorld`, sized by gear size, [gear-size.md](gear-size.md));
- the other hand pulled onto `off` by the MakeHuman body's two-bone IK (`SetOffHand`);
- the style's clips (`StyleClip`).

Third person (`UWeaponDrawSubsystem::UpdatePawn`): the weapon rides its holster until the pawn is ready (drafted, in a
fight, blocking, aiming, controlled, reloading, swinging), then moves into the hand and hangs from the hand bone. A gun
with a real model (`assets/guns.json`) is drawn with that model and its grip points. The upper body plays the style's
stance clip over the walk (`USimPawnAnimInstance::SetUpperStance`, from `spine_01` up), its aim clip on a target, its
block clip while blocking (the shield's with a shield on the arm), its reload while reloading. Cvars: `Weapons.Hands`,
`Weapons.Stances`, `Weapons.HolsterIdle`, `Weapons.GunModels`.

First person (`UGunRangeSubsystem`, now on in the main game too, `View.Arms`): a gun with a model is drawn as the gun
range always did. Anything else (melee weapons, guns without a model, shields, bare fists) goes through its hold style:
the style's `fpStance`/`fpBlock` clip, the sim's swing clip (`motionClip`) played on the arms at the sim's moment and
rate, the hands raised into view while ready (off through a swing, whose clip moves them), the weapon on the right
hand's grip, the other hand on a two-handed grip, the shield on the left forearm (raised before the face while
blocking). Bare fists jab when a blow lands, the hands taking turns. Melee no longer switches the view to third person.

Guns in first person: the left hand rides a pump gun's fore-end back and forward (`pump` + `cycle` in guns.json), and a
reload keeps the barrel pointing ahead by `reloadSteady` (the shotgun 0.75) so its stock doesn't swing the muzzle
across the view.

## Carrying a body

sneed (2026-11-14): carrying a downed pawn is a backpack-style hunched carry with the ragdoll on the back. The sim says
who carries whom (`ThingView::carryingPawn` / `carriedBy`, from the rescue, capture and corpse-haul jobs and a
fighter's rescue). `UBodyCarrySubsystem` moves the carried ragdoll onto the carrier's back once and holds it there by
the chest with a physics joint to a point behind the carrier's shoulders (a little sway allowed); the arms, head and
legs hang loose. The carrier stoops (`SetSpineBend`, 26 degrees) and reaches back under the legs with both hands (arm
IK). Laid down, the joint lets go and the body falls where it is, on a ragdoll floor made there. `Pawns.CarryOnBack 0`
turns it off.

## Not done

- Crouched or prone there is no crouch clip yet (only the drawn height drops), so the weapon keeps its old lowered place
  there instead of the hands.
- No proper carry clip exists in the library; the stoop and the arm reach stand in. Bow drawing on the arms in first
  person uses the generic path (no arrow nocking yet).
- The unarmed swing clips named in MeleeRules (`Motion_Unarmed_*`) have no defs yet; fists use the procedural jab.

## Spear guard and thrust (sneed 2026-11-18)

The spear was carried point-up across the back (the SpearGuard mocap stance) and slashed like a greatsword. Now:
- **Pole grip** (`HoldGrip::Pole`, `HoldDraw::PoleFrame`): the shaft runs from the right fist through the left, so the
  stance decides where the point goes. The stance is the rifle idle (rear hand at the hip, front hand ahead): a low
  forward guard, point at the enemy.
- **Thrusts**: `MeleeRules.styleAttacks` takes a weapon def name as a key too, and a weapon's own list wins when any
  of its clips is measured (`MeleeSystem`, like `lowAttacks`). `Melee_Spear`: `Motion_Spear_Stab`, Mixamo's "Bayonet
  Stab" at chest height (Height 85; the low one, Height 0, is the blow at the downed), downloaded and measured on
  2026-11-19: 1.5 s (animation 0.35-1.85 s), blow 0.73 s in, the hips lunge 0.43 m forward and come back. The mocap fencing and
  sword-lunge thrusts were measured on 2026-11-19 (`Motion_Spear_Thrust`, `Motion_Spear_Lunge`) but are not used: those
  performers stand side-on, so the lunge travels to the pawn's right and would stab sideways.
- **Facing**: the rifle idle's hands laid the shaft across the body (PC picture 2026-11-19), so `PoleFrame` points the
  shaft where the holder faces, tilted by the hands' line, at most 10 degrees up and 20 down (the side picture showed the rifle idle's raised front
  hand holding it 35 degrees up, like a pike). With one hand on it, or the
  hands closer than 8 cm, it still points ahead, level (the next PC picture showed it standing upright along the leg,
  following the single fist).
- PC check: the range tour's step 15 pictures the spear in first and third person.

## Weapon first, then the hands (sneed 2026-11-19)

sneed: "have you fixed the awful first and 3rd person weapon holding yet to be realistic and accurate?" and "the arms
cannot be messed up, like unnatural bends in hands, forearms or otherwise". The old way placed the weapon where the
clip's hand happened to be, so the clip's arm decided where the gun pointed; in first person the whole arm rig was
turned until the barrel lined up (`AimCorrection`), which bent wrists and twisted forearms. Now it is the other way
round, one system for every weapon, both views:

1. **The weapon is placed first**, at its style's place for the moment: `places.ready`, `aim`, `block` (third person),
   `fpReady`, `fpAim`, `fpBlock` (first person; each falls back to the one before, then to the third-person one,
   `colony::holdPlaceFor`). A place says which body point it hangs from (`anchor`: Chest = between the shoulders,
   Head = the eyes, View = the camera), which weapon point sits there (`point`: Main grip, Sight, Stock), where (`at`,
   cm: ahead, right, up) and how the weapon is turned (`turn`: pitch, yaw, roll). A gun aimed puts its **sight on the
   eye line** (first person: on the screen's centre, so iron sights line up); ready, a rifle's **stock sits in the
   shoulder pocket**. A weapon def can override any place under `hold.places`. Places blend over a quarter second.
2. **Both hands are pinned to the weapon's grips** (`HoldDraw::PinHands` / `ApplyPinned`): the main hand on `main`,
   the other on `off` turned by `offTurn` (a rifle's palm up under the fore-end), or for one-handed guns on `support`
   (the other hand cupping the grip, a two-handed pistol hold; `support` is measured from the main grip, so it follows
   wherever a pistol's grip point sits).
3. **Swings and full-body clips free the main hand** (third person: the clip moves the arm and the weapon stays in the
   hand; the off hand follows it). First person melee swings move the *weapon* along an arc (windup, strike, back to
   ready, timed by the sim's clip) and the hands follow. A third-person swing with no clip playing on the body moves
   the weapon along the procedural swing path with both hands kept on it, so a weapon never hangs still in the air
   (sneed 2026-11-19: the club and longsword floated and didn't move on a swing).
4. **Shooting from the hip raises the gun to the eye** for as long as the gun's cooldown runs (it starts at the first
   shot), so the arms follow the shots; aiming up or down turns the Head-anchored place with the view's pitch.

Per weapon there is still only the `hold` block; grenade and rocket launchers need only their grip points (and a
place if they sit differently from a rifle).

## Arms that can't bend wrong

Both hands go through one arm solver, `FMakeHumanArmsNode` (`MakeHumanAnimInstance`), which does its geometry in the
sim's plain C++ `colony::arm` (`ArmPose.h`), so it is unit tested:

- **The elbow is a hinge**: it bends 0 to 145 degrees on its own anatomical axis (read from the body's bent rest pose),
  never backwards; the upper arm rolls so the hinge lies in the new bend plane.
- **The elbow points down, out and a little back** (`naturalPole`), as people hold a weapon.
- **No stretching**: a grip past the arm's reach first brings the shoulder forward (the clavicle turns up to 25
  degrees, as a person reaching out does); still out of reach, the hand stops at 99.5% of the arm's length, short on
  the line to it (and the first-person rig leans forward up to 25 cm until the shoulders reach, `ArmsLean`).
- **The chest turns to reach** (up to 40 degrees about the body's up axis, the neck turning back so the head keeps its
  look): a long gun's fore-end is past the far arm with square shoulders, so the body blades as a shooter's does.
  Still out of reach, the **support hand slides back along the weapon** towards the grip hand, at most 15 cm.
  Long guns at the ready are held low (grip before the belly, stock tucked under the arm, muzzle down and across the
  body) and the spear's guard has the rear hand at the hip, so both hands reach on every body (PC run 2026-11-19).
- **The weapon stays at its place** while the hands hold it there (third person too): a hand the wrist limits hold
  back never turns the weapon; the other hand goes to its own grip at the place, not after the main hand. Only a swing
  or a full-body clip carries the weapon in the hand.
- **Wrist and forearm limits**: forearm twist 85 degrees either way from thumb-up, wrist bend 70, sideways 28
  (`Limits`). A hand turned further is brought back to the nearest pose a wrist can take (`clampHand`); the weapon
  stays where it was placed and the grip gives a little instead. A backwards elbow reads as 180 degrees of twist, so
  the same limit catches it.
- **The elbow swings to fit the grip** (`chooseSwivel`): a grip fixes the hand's turn, so the elbow is swung round
  the shoulder-wrist line (up to 100 degrees from its natural down-and-out) to where the forearm lets the wrist take
  that turn inside its limits, as a rifleman lifts the trigger arm's elbow out to the side. A swing costs a little, an
  elbow over the shoulder or in across the chest costs more, and a change from last frame's swing costs a little too
  (no hopping). The support hand may also **turn round the fore-end or shaft** (it is round; a pistol's cupping hand does not turn): up to 90 degrees on a
  gun, 180 on a spear, 45 on a two-handed hilt, chosen with the elbow's swing. PC round 7 measured why: the grips
  asked the right wrist for 54 to 96 degrees of sideways bend (limit 28) and the left forearm for 130 of twist (85).
- **Fingers close round the grip**: a pinned hand's four fingers get only the curl its clip lacks, towards the palm,
  up to a natural fist (index 50/60 degrees at its first two joints, the little finger 65/65, the last joint a third as
  much); the thumb stays as animated. An open-handed clip no longer leaves the hand holding air beside the gun.

Proof: `ArmPoseTests` solves 20,000 random grips round the body (out of reach, against the shoulder, any hand turn)
and every one is inside the limits. On the PC, smoke test `gunrange` measures **both arms of every drawn frame** (the
first-person arms and the third-person body), from the solver and from the drawn bones, and fails on any joint out of
range (`arm_limit_fails`), and on more than 1% of hand-frames with a hand over 3 cm off its grip (`arm_hand_short`, the `HAND OFF ITS GRIP` notes give the gun, view, hand and where the goal was); its step 16 pictures every gun hip, aimed, third-person ready and aimed (`hold_<Gun>_*`).
Cvars: `Pawns.ArmLimits`, `Weapons.Places`, `Weapons.PlacesFirstPerson` (0 = the old way, for comparing).

## Never floating, and aiming at the crosshair in third person (sneed 2026-11-20)

sneed: "a bug ... makes the graphic for melee attacks screw up (and I drop the weapon) but I keep it. The weapon then
floats but the hand animations still work"; "if I swap weapons, go unarmed and back and press Z the weapons can bug and
float"; "I like the ability to aim in 3rd person but the weapon doesn't move with my cursor to aim, and arms".

- **Crouched and prone.** The weapon used to leave the hand below 0.9 m body height (crouch C, prone Z) for an old
  fixed spot in the air. Now the hands hold it at any height; the weapon-first pinning runs only when standing.
- **The float guard.** Each frame a weapon fully in the hands measures how far its grip is from its hand bone. More than
  22 cm (`WeaponFloatCm`, scaled by body size) for 0.1 s puts it back on the hand's own grip frame and holds it there
  0.6 s (`ForceHandSeconds`), whatever drew it away (a swing path, a pinned place the arm can't reach, a switch).
  The log says `WEAPON FLOAT` once per pawn with what was going on, so a cause shows up in play.
- **3rd person aim.** While aiming a gun in third person the body turns to the camera's yaw, and the aimed place is
  turned (twice, from the sight) so the sight line meets what the crosshair is on, up to 60 degrees round and nearly
  straight up or down; the arms follow because the hands are pinned on the weapon. Smoke `finishers` phase `aim3p`
  checks the rifle within 8 degrees of the crosshair and the body within 20 degrees of the camera.

## Stock clear of the head (sneed, 2026-11-22)

The shotgun's stock went through the head when aiming. For two-handed guns, after the aim blend the gun slides forward
along its barrel in 1 cm steps (up to 12 cm) until the stock-to-grip line clears a 7.5 cm skull round a point 9 cm
above the head bone (both scaled by body size). `GetStockSlideCm` reports it; the gun range smoke notes it. In melee,
the feet now turn with the body through a swing (`bTurnFeet`), so a turning mace swing no longer twists the spine up to
60 degrees and the weapon follows the body.
