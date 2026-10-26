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
   (0.6 s) for fists, 15 for a knife. The step the swing starts in counts as the first. The attacker
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

**Where it goes** (the attacker's skill): at the commit, the target's chest (`aimPoint`) moved by a
random point in the circle around it that a standing man seen from the front fills with the
attacker's **melee hit chance** (RimWorld's, below), from the same table aimed shots use
(`CombatSystem::spreadRadiusFor`). Being level and from in front, the strike sees the man the way
the table does, so a standing man is hit about as often as RimWorld says (test: 51% at skill 0
against RimWorld's 50%, 94% at skill 20 against 90%). The player's own swing has no spread.

**Damage** goes through `CombatSystem::hitPawn`, the same code as a bullet's: the part under the
hitbox (a hand or a finger), in to an organ or bone if the damage type reaches inside (cuts and
stabs do), then armor outside layer first (half of what armor stops is deflected, "deflected"; the
rest does half damage, sharp turned blunt), then `HealthSystem::applyHit`.

## Hit and dodge chance (RimWorld's)

From the RimWorld wiki, in `MeleeRules`:

| | Base score | Curve |
| --- | --- | --- |
| Melee hit chance | Melee skill + 12 x (Manipulation - 100%) + 12 x (Sight - 100%) (each counted up to 150%) + traits (`meleeHitChance` offsets: Brawler +4) | 0: 50%, 10: 80%, 20: 90%, then +0.3% a point |
| Melee dodge chance | Melee skill + 18 x (Moving - 100%) + 8 x (Sight - 100%, up to 140%) + traits (`meleeDodgeChance`: Nimble +15) | up to 5: 0%, 20: 30%, 60: 50% |

Pawns without skills count as Melee 4 (62% hit, no dodge). The test checks the wiki's tables.

## Defending: dodges and blocks with timing

When a swing starts at a pawn that isn't controlled by the player, isn't downed and isn't busy (its
own wind-up, or aiming or firing: RimWorld's "no dodging while aiming or firing"), it **reads** the
swing after its reaction time: 14 steps at Melee 0 to 5 at Melee 20 (`reactionStepsBySkill`), plus or
minus 4 picked per swing (ours). Then:

- **Dodge**, as often as its dodge chance says, if it is a fighter (the job system walks other
  pawns). It moves when the attacker commits (if it moved sooner the swing would follow it), or as
  soon as it reads the swing if that is later: a one-cell sidestep to whichever side is free
  (`PawnAction` `Dodge`, `target` the attacker, `cell` where it steps). Whether it gets clear is up to
  time and room: a late read, a fast weapon, a hurt leg or a wall at its side and the strike lands
  anyway. One that got clear is reported as "dodged". The fighter AI waits until the swing is over.
- **Block**, otherwise, if it holds a melee weapon: with a chance of 10% at Melee 0 to 50% at
  Melee 20 (ours; RimWorld has no blocking). The weapon is up `blockSteps` (4) after it reads the
  swing, until the hit (`PawnAction` `Block`, `target` the attacker; read too late, nothing happens).
  A strike that meets a blocking pawn **from the front** (the blocker faces the attacker) does no
  harm ("blocked"), and the blocker staggers: `blockRecoverySteps` (12) of recovery before it can
  swing itself. From behind, a raised weapon doesn't help.

The player defends by hand: `MeleeCommand` `Block` raises the weapon for `playerBlockSteps` (20)
after `blockSteps`, and moving away (DirectMove) is the player's dodge. The controlled pawn never
defends by itself.

In the tests a skill 20 fighter sidesteps 26% of swings and gets clear of nearly all of them; a
skill 20 blocker raises its gladius against 40% of swings and catches nearly all of those.

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
