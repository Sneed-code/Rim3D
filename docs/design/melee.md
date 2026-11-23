# Melee: swings, reach, blocks and dodges

Code: `ColonySim/Melee.h`, `Private/Melee.cpp`; the fighter AI's part in `Private/Fighters.cpp`
(`planCharge`, `meleeAct`). Tests: `Tests/MeleeTests.cpp`. Data: `tools` on the melee weapons
(`Things/MeleeWeapons.json`, D8) and on pawn defs (fists, teeth, claws), `Rules/MeleeRules.json`.
Task F8. It stands on guns, hitboxes and armor ([combat.md](combat.md)), the fighter AI
([fighters.md](fighters.md)) and health ([health.md](health.md)).

The game design doc: "Only charges into melee when armed for it, or when the trait (such as Brawler)
says so. In melee, swings, blocks and dodges with timing." As with guns, the sim decides and Unreal
shows: every swing is timed in the sim, and a hit is a real line against hitboxes, not a dice roll on
a sheet.

## What a pawn fights with

A melee weapon (an item def with `tools` and no gun verb) goes in the hands like a gun: `Equip`
puts it in `Gear::weapon` and drops what was there. A pawn swings its weapon's tools; with a gun or
nothing in hand it uses its body's (`tools` on the pawn def: a Human's left fist, right fist and head,
an animal's teeth and claws). Each tool has (D8, [starting-data.md](starting-data.md)):

- `capacities.0`: the damage type (Cut, Stab, Blunt, Bite, Scratch).
- `power`: the damage.
- `cooldownTime`: steps for the whole swing (RimWorld seconds x 30), times the weapon's
  `meleeCooldownMultiplier`. Keeping the whole swing at the tool's cooldown keeps RimWorld's damage per second.
- `armorPenetration`: if the tool gives none, `power` x 0.015 (`armorPenetrationPerDamage`, RimWorld's
  default; unverified).
- `chanceFactor`: a swing picks a tool by chanceFactor x power (ours: RimWorld also favours the
  strongest tool, by a rule the wiki doesn't give).

Stuff and quality don't change melee damage yet (items have no stuff).

**Reach** (ours, `MeleeRules`): fists and other body tools 0.85 m from the pawn's centre (an animal's teeth
and claws 0.65 m + 0.4 x the root of its body size, N5: [animal-fights.md](animal-fights.md)); knife 1.0;
club, gladius and mace 1.2; breach axe 1.3; longsword 1.5; spear 1.9 (`weaponReach`, by defName).

## A swing

`MeleeSystem::attack(pawn, target, player)` (the fighter AI, or `MeleeCommand`) starts one:

1. **Wind-up**: 30% of the tool's cooldown (`windupShare`, at least `minWindup` 9 steps): 18 steps
   (0.6 s) for fists, 15 for a knife, at Melee skill 10. The whole swing (wind-up and recovery) is divided by the
   skill's speed, 0.8 at skill 0 to 1.2 at skill 20 (`skillSpeedBySkill`, sneed's), and multiplied by worn armour's
   attack time factor: fists wind up 23 steps at skill 0, 15 at skill 20. The step the swing starts in counts as the first. The attacker
   turns to follow the target.
2. **Commit**: `commitSteps` (6, 0.2 s) before the hit the strike's line is fixed. Before that the
   swing follows the target; after it, a target that moves can get out of the way.
3. **Hit moment**: the strike (below). A `MeleeHit` event says what came of it.
4. **Recovery**: the rest of the cooldown. No new swing until it is over.

A swing starts only at a living pawn whose centre is within the reach plus 0.1 m (`reachSlack`: its
front is in reach), and not while the pawn is downed, mid-shot (aiming or firing a burst) or still
swinging or recovering. A downed attacker's wind-up is off.

## The strike

The strike is a level line from the attacker's centre at the height of the aimed point, through it
and 0.35 m on (`strikeDepth`), but never further than the reach. It is traced like a bullet's flight
(`CombatSystem::trace`): the first hitbox it meets is hit (a friend in the way too), and a wall in the
way stops it ("missed"; walls take no damage from melee yet). A pawn without hitboxes (animals until
they get theirs) is hit if the line passes within 0.3 m of its centre, on a part health picks.

**Where it goes**: at the commit, the target's chest (`aimPoint`) moved by a small wobble of the hand, a random
point within 12 cm at Melee 0 to 3 cm at Melee 20 (`aimWobbleBySkill`, ours). No hit chance: a man standing still
is hit at any skill (test: at least 97% at skill 0 and 20). The player's own swing has no wobble.

**Damage** times the skill's factor, 0.8 at skill 0 to 1.2 at skill 20 (`skillDamageBySkill`, sneed's), with the
weapon's quality and stuff. It goes through `CombatSystem::hitPawn`, the same code as a bullet's: the part under the
hitbox (a hand or a finger), in to an organ or bone if the damage type reaches inside (cuts and
stabs do), then armor outside layer first (half of what armor stops is deflected, "deflected"; the
rest does half damage, sharp turned blunt), then `HealthSystem::applyHit`.

## RimWorld's hit and dodge chance: gone

RimWorld's melee hit chance and dodge chance tables drove the spread and the dodge roll until 2026-11-16. They are
gone (decision 0010); skill now shows as speed, damage, a steadier hand and quicker reading of swings.

## Defending: dodges and blocks with timing

When a swing starts at a pawn that isn't controlled by the player, isn't downed and isn't busy (its
own wind-up, aiming or firing, or within `recoveryNoDefendSteps` (12) after its own strike or `dodgeRecoverySteps`
(30) after a sidestep), it **reads** the swing after its reaction time: 14 steps at Melee 0 to 5 at Melee 20
(`reactionStepsBySkill`), Nimble 3 quicker (`reactionTraitSteps`), plus or minus 4 picked per swing. Read before
the hit, it does the sensible thing, no roll:

- **A shield on its arm** (a fighter): nothing here; the fighter AI raises the shield ([shields.md](shields.md)).
- **Parry**, if it holds a melee weapon and its guard isn't broken: the weapon is up `blockSteps` (4) after it reads
  the swing, until the hit (`PawnAction` `Block`, `target` the attacker; read too late to raise it, nothing happens).
  A strike that meets a blocking pawn **from the front** (the blocker faces the attacker) does no harm ("blocked"),
  and the blocker staggers: `blockRecoverySteps` (12) of recovery before it can swing itself. From behind, a raised
  weapon doesn't help. **Guard**: the third parry in a row, each within `guardWindowSteps` (150) of the last
  (`guardBlocks` 3), breaks the guard: a stagger of `guardBreakSteps` (75) in which it can't parry (the `MeleeHit`
  has `count` 1). The next blow gets through unless it sidesteps.
- **Sidestep**, otherwise, if it is a fighter (the job system walks other pawns): it moves when the attacker commits
  (if it moved sooner the swing would follow it), or as soon as it reads the swing if that is later: a one-cell
  sidestep to whichever side is free (`PawnAction` `Dodge`, `target` the attacker, `cell` where it steps). Whether
  it gets clear is up to time and room: a late read, a fast weapon, a hurt leg or a wall at its side and the strike
  lands anyway. One that got clear is reported as "dodged".

The player defends by hand: `MeleeCommand` `Block` raises the weapon for `playerBlockSteps` (20)
after `blockSteps`, and moving away (DirectMove) is the player's dodge. The controlled pawn never
defends by itself.

In the tests a skill 20 unarmed fighter gets clear of over three quarters of a skill 20 club's swings, a skill 0 one
of under half as many; a skill 20 parrier catches every swing its guard holds for, and every fourth gets through.

## The fighter AI

Hooks in `FighterSystem` (`setMelee`):

- **Who charges** (`MeleeSystem::charges`): no gun in hand, and a melee weapon, or a trait in
  `chargeTraits` (Brawler). A charger's plan doesn't look for cover: its spot is the cell where its
  target was last seen, and it re-plans the chase twice a second (keeping on along the old route while
  the new path is worked out). A drafted charger goes only as far as its leash allows (plus its reach
  and a metre), unless the player gave the attack order. A charger that is falling back falls back as
  before.
- **Swinging** (`meleeAct`, every step before the shooting part): mid-wind-up a fighter holds still.
  A fighter without a gun swings whenever it is ready at its target if it is in reach, else at the
  nearest able enemy in reach. A charger walks on until the target is well inside its reach (reach
  less 0.2 m, at least 0.7 m), so a knife closes in to where fists can answer it, and a spear stays
  at its length. A fighter with a gun shoots, even point blank.
- **Colonists hold with the others** (the Disciplined profile, [real-time-combat.md](real-time-combat.md)): a
  drafted or defending colonist armed for melee charges only an enemy within `ambushRange` (6 m) or one already at a
  friend, unless the player ordered the attack ([fighters.md](fighters.md)). Raiders charge.
- **Unarmed fighters hold.** A fighter with no gun no longer walks toward its target to "get in
  range" (the range terms of the spot score need a gun): it keeps cover and strikes back at
  whoever comes into reach.
- **No running from arm's length.** A fighter falling back with an enemy within its reach plus 1.5 m
  (`standGround`) stands and fights instead: at the same speed nobody gets away on foot, and a chase
  would go round in circles for ever.

## Commands, events, snapshot, saves

| | |
| --- | --- |
| `MeleeCommand{pawn, target, mode}` | `Attack`: swing at `target`; the controlled colonist may swing with no target, straight ahead (U22's melee input). `Block`: raise the melee weapon |
| `MeleeSwing` | `thing` the attacker, `target`, `def` the weapon's def (or the pawn's for fists), `message` the tool ("blade", "left fist"), `amount` steps to the hit |
| `MeleeHit` | `thing`, `def`, `target` (the pawn struck, or the one aimed at), `hit` (Pawn, Armor, Building, None), `part`, `amount` (damage after armor), (x, y, z) where the strike ended, `message` "hit", "deflected", "blocked", "dodged" or "missed"; health events follow a hit |
| `PawnAction` `Block` | a pawn raised its weapon (`target` the attacker, none for the player's own block) |
| Snapshot | `meleePhase` (Ready, Windup, Recovery), `meleeStepsLeft`, `meleeTarget` (while winding up) and `blocking` on each `ThingView` |
| Save | section `melee` (version 1): the `Melee` component of each pawn that has swung or been swung at |

U22 draws it ([melee-draw.md](melee-draw.md)): a swing animation stretched over the wind-up so the blow lands on the `MeleeHit`
step, a hit-stop and flinch on "hit", the raised weapon while `blocking`, the sidestep on `Dodge`.

## Strong swings (AN1)

For the animation only: a swing is **strong** when its tool's power is at least `strongPowerShare` (1.15, ours) of
the mean power of the tools it chose from, such as a longsword's blade (19.4) against its handle (12). Fists and the
head are never strong (8.2, 8.2 and 5). The MeleeSwing event's `count` is 1 for a strong swing; the snapshot has
`meleeTool` (the tool's index) and `meleeStrong`. Unreal adds its own light and heavy variety for fists.

## Not done yet

- Melee skill XP, stuff and quality multipliers on damage, weapons wearing down.
- Melee against buildings (doors, walls) and against downed pawns by choice (the AI takes the
  downed only when nobody else is left, as with guns).
- Animals fighting (N5 gives them hitboxes and the predator AI; the strike already reaches pawns
  without hitboxes).
- Social fights (C20) start swings through `MeleeSystem::attack` with fists.
- Crashlanded's knife: the starting items don't include it yet (it would change every replay's
  hashes; add it with the next task that changes the start anyway).
- Several weapons and switching between a gun and a melee weapon by range (P1).

## Pace and step-in (sneed, 2026-11-23)

- Every swing takes `attackTimeFactor` (0.75) of its RimWorld time: "all melee attacks 33% faster". Finishers and the
  clips (played timed to the hit step) speed up with it.
- A person's bare fists take a further `unarmedTimeFactor` (0.25) and hit with `unarmedPowerFactor` (0.25) of the power:
  four times the punches, each weak, the same damage per second. Animals' bites and claws are unchanged.
- `unarmedMinWindup` (4 steps) is the shortest wind-up for fists (`minWindup` 9 for weapons, both times the pace factor).
- The player's swing from standing (slower than `stepInMaxSpeed`) is the whole body's: the clip's step forward carries
  the feet into a thrust instead of the chest leaning. Walking off during the wind-up or recovery hands the clip to
  the upper body (`stepInFreesFeet`, `MeleeSystem::freeFeetToWalk`), so stopping or moving mid-swing stays seamless.
  Set `playerStepIn` and `stepInFreesFeet` to false to go back to the arms-only swing of 2026-11-18.
