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
  of its clips is measured (`MeleeSystem`, like `lowAttacks`). `Melee_Spear`: `Motion_Spear_Thrust` (one lunge out of
  the mocap fencing loop) and `Motion_Spear_Lunge` (mocap clip 13). Both still need their trim and blow picked on the PC
  (`-run=MotionClips -Trace=Motion_Spear_Thrust,Motion_Spear_Lunge`, then `from`/`to`/`hitSeconds` in
  `assets/motion_clips.json`); until then the spear swings its style's clips.
- PC check: the range tour's step 15 pictures the spear in first and third person.
