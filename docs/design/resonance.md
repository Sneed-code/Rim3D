# Resonance: the attuned and their powers

Code: `ColonySim/Resonance.h`, `Private/Resonance.cpp`; `Stunned` in `Combat.h`, `HealthMods` in
`Health.h`. Tests: `Tests/ResonanceTests.cpp`. Data: `data/defs/Powers/Resonance.json` (the
powers), `data/defs/Rules/ResonanceRules.json`. Task X2. It stands on health ([health.md](health.md)),
the fighter AI ([fighters.md](fighters.md)) and the job system ([jobs.md](jobs.md)).

Resonance is the setting's magic ([lore.md](lore.md)) and our name for RimWorld's psycasts; the words
are in [naming.md](naming.md). An **attuned** pawn has:

| Ours | RimWorld | What it is |
| --- | --- | --- |
| attunement level, 1 to 6 | psylink level | which powers it can use; the strain limit |
| strain | neural heat | every power adds to it; it falls by itself; a power that would take it past the limit is refused |
| focus, 0 to 1 | psyfocus | every power spends a little; it fades slowly |
| resonance sensitivity | psychic sensitivity | 1 for everyone now; a def's `stats.resonanceSensitivity` and a gene's `ResonanceSensitivity` stat change it; 0 is deaf (nothing can be done to it) |

## Numbers (RimWorld's, from the wiki)

- **Strain limit** (base + level) x 10 x sensitivity: 30 at level 1, 60 at level 4, 80 at level 6.
- **Strain falls** 0.54 a real second at level 1 (0.018 a step), 12.5% faster for every level above
  1, and 3x the pain on top (a pawn in 100% pain cools four times as fast).
- **Focus fades** per game day by the band it is in: 3.5% below 25%, 5.5% to 50%, 7.5% above.
  Powers of level 3 and 4 need at least 25% focus, level 5 and 6 at least 50%. A pawn attuned for the
  first time starts at 50% (`startFocus`, ours: meditation, which fills focus, isn't built yet).
- Ticks become steps (/2), tiles become metres, as for guns ([combat.md](combat.md)), so a stun is 3
  real seconds at 1x as in RimWorld.

## The powers

| Power | RimWorld | Level | Strain | Focus | Range | Does |
| --- | --- | --- | --- | --- | --- | --- |
| **Stun** | Stun | 1 | 12 | 1% | 24.9 m | the target can't walk, shoot, throw, swing, block, dodge, cast or work for 90 steps x its sensitivity |
| **Numb** | Painblock | 1 | 8 | 2% | 24.9 m | the target's pain x 10% for 3,600 steps x sensitivity: a pawn down from pain alone gets up |
| **Burden** | Burden | 1 | 8 | 1% | 29.9 m | the target's Moving is capped at 40% (a cap, not a factor) for 600 steps |
| **Jump** | Skip | **4** | 25 | 2% | 27.9 m | the target is moved at once to a cell it can see within range of the caster; not pawns bigger than body size 3.5 |

Every power has a warm-up of 8 steps (RimWorld's 15 ticks) and needs a line of sight from the
caster to the target (and, for a jump, from the target to the cell). Numb and jump may be used on
oneself. **Jump is level 4**, as RimWorld's skip is, so a pawn needs attunement level 4 for it; the
stage plan named it with the first-level powers, but the level is RimWorld's number. Changing the one
`level` in `Powers/Resonance.json` makes it a first-level power.

## Getting powers

- `AttuneCommand{pawn, level}` sets the level (0 takes attunement away). Every new level reached
  teaches one power of that level the pawn doesn't know, at random (RimWorld's rule), picked from the
  game's seed, the pawn and the level. It stands in for RimWorld's psylink neuroformer and titles.
- `LearnPowerCommand{pawn, power}` teaches any power of the pawn's level or lower (RimWorld's
  psytrainer).

## Casting

`CastPowerCommand{pawn, power, target, cell, player}`. Refused, with a reason, when the pawn isn't
attuned or doesn't know the power or its level is too low; when it can't act (downed, knocked down,
stunned, casting, firing a burst, winding up a throw, mid-swing, rolling, switching weapons; a pawn
that is only aiming may cast, and its aim is dropped); when focus is short or strain would go past the
limit; when the target is not a living pawn on the map, is out of range or sight, is deaf, or is the
caster for a power that can't be used on oneself; for a jump, when the cell can't be stood on, is out of
range, or the target can't see it.

Then the pawn winds up (`Casting`, a `PawnAction` `Cast`), facing the target. It stands still: the
job system pauses its job (`JobSystem::setPauseGate`), a fighter stops and plans again afterwards, a
player's DirectMove is ignored. At the end the target, range and sight are checked again; if they
fail the power fizzles at no cost. Otherwise strain and focus are paid, the effect starts and a
`PowerCast` event says so. A pawn stunned, downed or knocked down while winding up loses the cast.

**Effects.** A stun is the `Stunned` component (in `Combat.h`, so every system can check it without
knowing resonance): the movement system holds the pawn, shooting, throwing, switching weapons and
setting a stance are refused and a shot or throw being prepared is dropped, melee can't swing, block
or dodge, the fighter AI skips it, its job waits. Its stance stays as it was. Numb and burden are
`ResonanceEffects`; health asks for them whenever it works a pawn out (`HealthSystem::setMods`, a
`HealthMods` with a pain factor and a Moving cap), and a change marks the pawn's health dirty so the next
step works it out again (getting up or going down shows as `PawnGotUp` / `PawnDowned`). A jump puts the
target in the cell's centre, clears its walk, makes a fighter plan again and ends a worker's job
("jumped").

## The fighter AI

Every 15 steps (spread by pawn id) an attuned fighter in Fight mode that can act looks at its powers, in
this order, and uses the first that fits (all thresholds ours, in `ResonanceRules`):

1. **Numb** the friend (itself included) in the most pain, if it is 35% or more (`aiNumbPain`) and not
   numbed already.
2. **Jump** away when it wants out and an able enemy is within 4 m (`aiJumpThreat`). It wants out when
   it falls back, or would (outnumbered or badly hurt, the fighters' own test) but stands and fights
   because the enemy is at arm's length and nobody gets away on foot ([melee.md](melee.md)). It goes to its
   fall-back spot if that is 5 m or more away (`aiJumpMin`), else 12 m (`aiJumpEscape`, then less, down
   to 5 m) straight away from the enemies close by, or up to 90 degrees to either side.
3. **Stun** a known able enemy within 4 m of one of its side (`aiStunReach`), else one that is aiming
   or firing; nearest first.
4. **Burden** a known able enemy within 12 m (`aiBurdenRange`) that is walking.

Strain is never pushed past the limit, so a caster with powers keeps using them as strain falls.
Colonists who are not fighters don't use powers by themselves.

## Snapshot, events, saves

- `Snapshot::attuned`: per attuned pawn its level, strain, strain limit, focus and powers.
  `ThingView`: `stunnedFor`, `numbed`, `burdened`, `castPower`, `castStepsLeft` for any pawn.
- Events: `PawnAction` `Cast` when a warm-up starts, `PowerCast` when a power goes off
  ([bridge.md](bridge.md)).
- Section `resonance` v1 ([saves.md](saves.md)): `attunement`, `casting`, `stunned`, `effects` per pawn,
  powers by defName. All in `Sim::stateHash`. The three commands are in replays and saves by name.

## Not done yet

- Meditation (focus comes back), meditation spots and focus types; the anima tree.
- Pushing strain past the limit, overload and RimWorld's psychic breakdown.
- The other powers: chunk skip, solar pinhole, the level 2 to 6 powers, resonance drones and calms.
- Goodwill: using a hostile power on a neutral or allied faction's pawn (RimWorld: -15).
- Attuned raiders and Concord nobles from the pawn kinds; the protagonist's power keys and an aim
  preview; Unreal effects for casting, stuns and jumps (Unreal reads the snapshot and events above).
- Resonance sensitivity from traits (psychically sensitive, deaf) and the genes that change it.
