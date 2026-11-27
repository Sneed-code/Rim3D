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

Each press is one attack (no auto repeat). Kinds: Light, Heavy (Shift + click), Charged (keep holding the heavy:
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
  recovery); after `comboResetSteps` of no attack the combo starts over.
- **Cancel**: past `cancelShare` of the recovery a roll, a step or the guard ends the attack.

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

Title > Debug > **Sword and shield duel** (you against a mirrored hostile, healed before you go down, a new opponent
after each falls), and **Sword and shield duel (watch)** (both sides AI, screenshots, fails if nobody swings).

## Still to do

- The spear's flicking and jarring (the old swing path; it gets a moveset next).
- More movesets: spear, greatsword, fists, animals; big creatures.
- Damage, stats and scaling.
