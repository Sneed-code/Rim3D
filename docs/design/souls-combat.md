# Elden Ring style melee (souls combat)

sneed, 2026-11-27: "capture elden ring combat to a T", starting with a one-handed sword and shield. Stamina, light and
heavy attacks, combos, attacks that carry you in, jumping attacks, guard, staggers and knockdowns from poise, a poise
guard (hyperarmour), all built for every creature, not only people. Damage numbers and hidden stats come later.

Numbers: `data/defs/Rules/SoulsRules.json`. Code: `Souls.h`, `Souls.cpp` (in `MeleeSystem`), hooks in `Melee.cpp`,
`Combat.cpp` (roll and guard gates), `Fighters.cpp` (AI). Tests: `Tests/SoulsTests.cpp`.

## The switch

`SoulsRules.enabled` false turns all of it off: melee is exactly what it was before (the parry, jolts, auto-repeat
swings), the stamina bar goes, and the old tests run as before. Only pawns whose weapon (or race) has a **moveset** fight
the new way; everyone else keeps the old swing, and blows at a downed, lying or dead body keep the old low blows and
finishers.

## Stamina (every pawn)

A 100 bar (`Stamina` component, added on first use, removed when full again).

| Action | Cost |
| --- | --- |
| Light attack | 10 |
| Heavy attack | 20 |
| Charged, running, jump, counter | the kind's `stamina` |
| A hit taken on the guard | half the attack's cost (`blockShare`) |
| Roll | 30 |
| Bare fists / animals | 5 / 10 when no kind applies |

It refills over 2 s (`regenSeconds`) from empty, starting the moment you stop using it. A raised guard stops the refill
(sneed: "for now"). At 0 you can't attack; a hit on the guard that it can't pay for **breaks the guard**: the guard drops
and you stand stunned for `guardBreakSteps`. Only the player's bar is shown, green under a red health bar, top left.

## Attacks

Each press is one attack (no auto repeat). Kinds: Light (a click), Heavy (hold the attack button past
`Melee.HeavyHoldSeconds`, 0.22 s; sneed 2026-11-27, Shift + click is gone; its own clips, a big slash then an overhead blow),
Charged (keep holding the heavy:
the windup pauses at `chargeShare` until let go or `maxChargeSteps`; held `minChargeSteps` or more it is Charged),
Running (attack while sprinting), RunningHeavy, Jump and JumpHeavy (attack in the air: the jump gives way to the
attack's clip), Roll (attack within `rollAttackSteps` of a roll ending), GuardCounter (a heavy press within
`counterSteps` of blocking a hit).

- **Whole-body clips** from the moveset (`movesets.<key>.clips.<Kind>`, a list: the combo plays them in turn). The clip
  is timed so the blow lands at the clip's `hitSeconds`; its feet go where the clip takes them, so you don't walk freely
  while attacking (the slow-down sneed asked for).
- **Focus point**: the attacker turns with its target through the windup and is carried in (or back, `warpBack`) so
  that at the blow it stands at reach minus `idealInset`: the blade goes through the middle of the enemy instead of a
  free swing in its direction. The carry is at most the kind's `maxWarp` metres. The player swings at the locked-on
  enemy first, else the one in view; with nobody in reach the swing goes straight ahead at nothing.
- **Combos**: a press during an attack is kept (input buffer) and plays when the chain opens (`chainShare` into the
  recovery, 0.15 since sneed 2026-11-28: quick follow-ups); after `comboResetSteps` of no attack the combo starts over.
- **The opening** (sneed 2026-11-28: "fast attacks and quick follow ups, but a slight delay after you stop attacking that
  gives an opening"): when the recovery runs out with no next blow pressed, or a step walks out of it, the attacker is
  still committed for `openingSteps` (10, a third of a second): no attack, roll or guard. A press then is kept and plays
  when it is over. The fighter AI reads a foe in its opening as open (it attacks, and charges heavies).
- **Cancel**: past `cancelShare` (0.75) of the recovery a roll, a step or the guard ends the attack.
- **Smooth moves** (sneed 2026-11-28: "some attacks still jar and move weird, like the strong attack and the leap
  attack"): the clips were looked at frame by frame (Blender, the hips' path). The old second heavy began mid-air in a
  leap (a standing body shot off at 2.2 m/s) and the old leap drifted a metre sideways; now the heavy chain is a grounded
  overhead (`Motion_SS_HeavyDown`, the hips stay within 0.15 m) then the big slash, the running heavy and the AI's leap is
  the whole sword-and-shield leap from standing (`Motion_SS_Leap`: a crouch, up and 3 m straight ahead into the blow,
  from `aiLeapMin`-`aiLeapMax` 3.4-4.3 m), and the jumping heavy is the mid-air slam (it starts in the air, as the player
  is). The carry-in is at most 0.5 m for lights and heavies (was 0.8-1.2 m), spread over five sixths of the wind-up, so
  a blow leans in instead of gliding.

## Tactical, not spammy (sneed 2026-11-28)

sneed: "combat has to be more tactical where I time attacks right than spam click". All in `SoulsRules.json`:

- **Swings 25% slower** (the moveset's `speed` 0.75): "I can't even see the attacks".
- **A real recovery after every blow**: the next blow of a combo starts no sooner than `chainShare` (0.45) into the
  recovery, so three blows in a row are three separate commitments.
- **Timing**: a press during an attack's wind-up is too early (`earlyPressBreaksChain`): it is dropped and that attack
  won't chain (its recovery plays out, then the opening). A press from the moment the blow lands is kept and chains.
  Mashing the button gets one blow and an opening; clicking as each blow lands gets the combo.
- **Blocked**: a blow that meets a guard puts the attacker's next attack back `blockedDelaySteps` (5, 0.15 s) from when
  its combo would have opened, and the blocker's next attack 0.15 s past its block stun. Both sides, player and AI.
- **The shield's room** (sneed: "the shield can clip the enemy, make sure there's a barrier when you hold it up"): a
  raised shield keeps any body in front of it (within `guardCos`) `shieldRoom` (0.8 m) off, centre to centre, the two
  eased apart (half each, at most `shieldPushPerStep` 0.03 m each a step, never into a wall); an attack at a raised shield
  is lined up to stop at that distance too.

## Poise and the impale (sneed 2026-11-28)

"Hitting someone removes poise. Base is 10, a weak attack -2, a strong -6; at 0 it resets and can't drop again for 10 s;
when poise is down and you get an open hit you do the special attack, the impale: fast, the target stunned as you do it,
then staggering." `personPoise` 10; a hit takes `weaponPoise` (2) times the kind's `poise` (lights, running, rolling and
jumping 1; heavy, charged, running and jumping heavies and the guard counter 3). Broken, the meter refills and takes no
poise damage for `poiseImmuneSteps` (300, 10 s); the victim reels for `postureBreakSteps` (60, 2 s). **Any** press (light
or heavy) at a foe reeling from a poise or guard break within `criticalRange` is the **riposte** (the impale: the sword
thrust through the chest, `Motion_SS_Riposte`), at `kinds.Riposte.speed` 1.6: the victim is held through it, then
staggers on its feet for `staggerSteps` (no second riposte while it does). The charged and jumping heavies still knock
down when they break poise.

## Critical attacks (sneed 2026-11-27)

Elden Ring's crits (eldenring.wiki.gg "Critical Attack": a light attack on a stance- or guard-broken foe is a riposte, one
from behind a backstab; 2.5 to 4 times a normal blow, ripostes about 25% above backstabs; both lock the two together):

- **Riposte**: a light press at a foe reeling from a poise break or a guard break (`Melee.stunUntil`, not a block's own
  short stun), in front within `criticalRange`. The sword lunge (`Motion_SS_Riposte`) goes through the chest; damage
  `kinds.Riposte.damage` (3.2x).
- **Backstab**: a light press within `backstabRange` behind a foe (its facing and the way to you at a cosine under
  `backstabCos`, about 53 degrees either side of straight behind), not a big body. 2.6x.
- Both: the victim is caught for the wind-up and `criticalHoldSteps` (no guard, roll or swing), the blow can't be blocked or
  dodged and always meets the torso. A backstab throws the victim to the ground (a knockdown, no poise needed, no
  immunity); a riposte leaves it staggering on its feet (sneed 2026-11-28, above).
- **Ground stab**: any press at a foe knocked down but still in the fight: a stab down (`Motion_Knife_Stab_Low`), 1.8x.
- **At the downed: hack or impale** (sneed 2026-11-28: "one is to injure, that's a hack; the impale is when it's
  calculated the final attack kills them, a bit slower, not much"; "critical that the impale goes through the centre,
  the chest"). A downed (incapacitated) body gets the weapon's finisher (MeleeRules `finishers`): the sword's `OneHand`
  is the hack (the low slash clip). It names its impale in `lethal` (`OneHandImpale`): when the impale's blow (its stab,
  the weapon's quality, skill and traits, times its `damage` 2.0) would take what is left of the torso to 0
  (`MeleeSystem::impaleKills`), the impale plays instead: 0.95 s to the blow against the hack's 0.8, always in the torso,
  and it kills (armour can't save the body: it was chosen because it kills). Its body plays the overhead downward chop
  (`Motion_SS_HeavyDown`, standOff 0.7 m: raised high, bent over the body at the blow; the low stab clip stood upright at
  arm's length and the point stopped 25 cm short, PC round 1) while Unreal drives the blade point down along the Sword path into `spine_03` (the chest's centre) with the hand on it
  (`drawWeapon`); the path's blow is laid on the clip's. A corpse gets the hack. Smoke `finishers` has an impale phase
  (the point within 15 cm of the chest, the victim dead).
- The AI gets them the same way (its light press at an open or turned foe).

## Poise

`PoiseMeter`: a person has `personPoise`; creatures by body size (`poisePerBodySize`, at least `minPoise`; animals also by
melee power). Each hit takes the weapon's `weaponPoise` times the kind's `poise`. It refills fully `poiseRegenSteps`
(30 s) after the last hit.

- **Break**: a stagger (the attack under way is cut, `staggerSteps` stunned, the stagger animation on the whole body).
- **Knockdown** instead when the blow is a knockdown kind (Charged, JumpHeavy) or the second break within
  `doubleBreakSteps`; then `knockdownImmuneSteps` of no knockdowns (no chained knockdowns). Bodies of
  `noKnockdownBodySize` or more never fall (big creatures stagger only).
- **Hyperarmour** (Heavy, Charged, JumpHeavy, GuardCounter): during the windup hits take half poise
  (`hyperArmorTaken`) and don't interrupt unless poise breaks.

## Guard

Right mouse with a shield (or a weapon): hits from the front (`guardCos`) are blocked. A shield stops all of the damage
(`shieldNegation` 1.0) and wears; a weapon lets 40% through. The blocker pays stamina and has a short blockstun; the
attacker's next heavy press after a block of its own is a counter. Bare fists against a blade fall back to the old
forearm block.

## Lock-on

An enemy who swings at you in melee while you are locked on to nobody and face them (within about 60 degrees of the view,
6 m) is locked on to by itself; so is (sneed 2026-11-28, "for now") a melee enemy within 5 m who is locked on to you while
you hold a melee weapon, whichever way you face. Settings > Controls "Auto focus" turns both off (saved).

Locked on with melee (sneed 2026-11-28, docs/design/lock-on.md): every walk and run faces the target, even with the
sprint toggle on; holding the sprint key down runs off (Elden Ring). The lock no longer lets go when you turn and move
away, only when the enemy is beyond `focusBreakRange` (25 m), out of sight a while, or you let go.

## AI

`MeleeSystem::aiSoulsTurn`: under `aiStaminaFloor` the pawn backs off and lets its bar refill; it takes heavies against a
raised guard and as counters, charges at an open target, chains up to `aiComboMaxBySkill` swings by skill, and leaps in
with a running heavy from `aiLeapMin`-`aiLeapMax` metres. With a shield it raises the guard against a swing coming at it
(`aiGuardChance`).

## Adding a weapon or a creature

Add a moveset in SoulsRules.json: `weapons` (or a race defName for a creature), the clip lists per kind (MotionClips
defs, `assets/motion_clips.json`, made by `-run=MotionClips`), `weaponPoise`, `speed`. Nothing else changes: stamina,
poise, guard and AI are the same for everything.

## Testing

Title > Debug > **Sword and shield duel** (a downed opponent stays 10 s before the next walks in; two women drawn as random creator people in daylight; Esc brings up the menu;
you against a mirrored hostile, healed before you go down, a new opponent
after each falls), and **Sword and shield duel (watch)** (both sides AI, screenshots, fails if nobody swings).

## PC round (2026-11-28): how it looks

Checked on the PC with close looks (smoke `duel -DuelLooks`: guard with and without the shield, the stance from three
sides, a knockdown each way and the get-up, the impale, a light and a heavy jump attack, several shots each).

- **Guard**: a drafted swordsman between blows crouched in cover like a rifleman (FighterSystem), and a crouched body
  draws no stance and no held weapon, so both guards were gone (arms hanging). Now a melee fighter waits standing. With
  a shield the sword is held up at the right side behind the shield's edge (`places.shieldBlock`), and beside it in the
  stance (`places.shieldReady`) instead of across it; alone, the guard is the blade across the body (`places.block`).
- **Get-ups** (MotionRules `getUpClips`, `getUpTurn`): each fall gets up from the pose it ends in, read off the
  `-run=MotionClips` POSE lines: back and left falls end on the back (the Game Animation Sample's up-from-the-back,
  turned 25 and 106 degrees), forward and right falls face down (up-from-the-front, 5 and 101). The clips are cut at 3 s
  (standing by 2.75 s). A clip that starts lying is set back by its hips' whole offset (`PawnActor`), so the hips stay
  on the spot the fall left them and the body doesn't slide 35 cm as it ends.
- **Jump attacks**: the attack's clip has its own jump in it (the heavy's mid-air slam starts with the hips 1.6 m up),
  so on top of the jump's height the body shot up 70 cm at once. The height left from the jump is now fitted so the hips
  go on from where they are and eased out by the moment the clip's feet land (`ASimPawnActor::JumpFitsClip`); the light
  jump attack starts its clip already in the air (`Motion_SS_Jump` from 0.25 s).
- **The impale** (riposte): its lunge clip stepped half a metre to the right and the thrust went past the victim; it is
  now measured `straight` (only its forward travel). The reeling victim turns to meet the blade and stands still
  through the thrust; the impaler is carried in to `impaleStandOff` (0.65 m) so the blade goes through; at the blow the
  thrust holds in the body for `impaleHoldSteps` (0.4 s) while the victim doubles over the blade, then it is drawn out
  and the victim staggers back. Unreal turns the blade about its grip so its line runs through the victim's chest
  (`UWeaponDrawSubsystem::ImpaleAim`; within about 10 cm of spine_03 in the looks).
- **The shield**: the steel medium shield is a real CC0 round shield (shields.md "Models").

## Running lunges (sneed 2026-11-28)

sneed: "enemies chasing you while you back pedal never melee you, they have to stop before attacking. Make it so they run
into you and add a lunging attack while running, all weapons; the key thing is they don't stop running until you do. A
lunging attack is -6 poise." Numbers: `SoulsRules` `lunge*`, `kinds.Running` (poise 3, so 6 with `weaponPoise` 2).

- **Every weapon kind has one.** The sword's moveset has its own `Running` clip; every weapon still on the old swing gets
  a `lungeOnly` moveset that gives it only the running attack (the rest stays the old swing): one hand and clubs the
  sword-and-shield running slash (`Motion_SS_Attack3`), two hand (not spear) the forward greatsword slash
  (`Motion_TwoHand_Slash2`), the spear the straight impale lunge (`Motion_SS_Riposte`), the bare fists a lunging lead jab
  (`Motion_Unarmed_Jab`, `unarmed`). `DualWieldLunge` is ready for the style `DualWield` (no dual-wield weapons yet). The
  player gets the same by attacking while sprinting.
- **It runs into you** (`MeleeSystem::soulsLungeCarry`): the lunge starts at the pace the attacker ran at and, each step of
  its wind-up, is carried over its clip's own step at just the speed that brings it to stand right for the blow when it
  lands, on where the target will be by then (its speed away times the steps left); never faster than `lungeTopSpeed`,
  never changing by more than `lungeAccel`, never faster than it can still slow from (so it arrives instead of running
  through), stopping at a wall or trunk. After the line is fixed and through the recovery it only slows.
- **The AI** (`MeleeSystem::aiLunge`, from `FighterSystem::meleeAct` before any stop): at a target moving away faster than
  `lungeFleeSpeed`, or when running in itself (faster than `lungeRunSpeed`), from no further than its reach, the clip's step
  and the carry it can make (`aiLungeCarryShare` of the top speed) less the target's lead, with a clear straight line. A
  chaser now runs (not walks) the last 3 m after a target getting away (`FighterSystem::sprinting`), and after a lunge
  whose target is still getting away it cuts its recovery at `lungeChaseShare` and runs on (no opening). The leap
  (running heavy) is not used at one getting away: its step is fixed. Tests: `Tests/LungeTests.cpp` (a colonist backing
  off at 2.5 m/s took 2 to 4 blows in 20 s before, 6 or 7 now, with every weapon kind and fists).

## The jump's landing (sneed 2026-11-28)

"Whenever I jump the landing is messed up, I float then it glitches." Unreal only (`ASimPawnActor::StartJump`,
`AdvanceJump`, `StartJumpLanding`): the jump's clips are measured on the fly (`PawnJumpClipTime`, the foot bones): the body
leaves the ground when the take-off clip's feet push off; the landing clip starts early enough (and at the right moment
of itself) that its feet touch down exactly as the body reaches the ground (the fall loop's tucked feet were still up
when the body landed, then snapped down); it plays out standing or briefly when moving, and its legs stay its own through
the blend back out (the walk's foot locks grabbed the feet half way). Smoke `jump` measures the feet against the ground
every frame (standing, walking, running, sprinting).

## Still to do

- The spear's flicking and jarring (the old swing path; it gets a moveset next).
- More movesets: spear, greatsword, fists, animals; big creatures.
- Damage, stats and scaling.
