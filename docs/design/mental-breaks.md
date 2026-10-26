# Mental breaks

Code: `ColonySim/MentalBreaks.h`, `Private/MentalBreaks.cpp`. Tests: `Tests/MentalBreakTests.cpp`; the
colony scenario prints a `mentalBreaks` count. Data: `MentalBreaks` defs and `MentalBreakRules`
(D9, [thoughts-data.md](thoughts-data.md)), `MentalBreakBehaviour` in `data/defs/Rules` (ours). Task
C19. Builds on [mood.md](mood.md) (the break bands) and [jobs.md](jobs.md).

A colonist whose mood is under a break threshold may snap. While the break lasts the job system
leaves the colonist alone (`JobSystem::step` skips a pawn with an active `MentalBreaker`, like a
fighter) and `MentalBreakSystem` walks it. When the break is over the colonist goes back to the
think list.

## When a colonist snaps

Every pawn with a mood, every `checkEverySeconds` (216, RimWorld's 150 ticks, staggered by the pawn's
id), in a break band (minor, major, extreme; the band the mood system keeps) gets one roll:

- the mean wait is `meanDaysBetweenBreaks` of its band (4, 1.5, 0.5 days) times the `moodCurve` at
  *mood / that band's threshold* (0.1 at mood 0, 1 at the threshold), so the lower the mood the sooner;
- the chance of the roll is `checkEvery / mean wait` (plain arithmetic, no `exp`, so every platform
  rolls the same);
- it needs: not asleep (`restOnlyWhenAwake`), not downed, on the player's side, not walked by the player
  (`PlayerControl`), and at least `minHoursBetweenBreaks` (24) since the last break ended.

Then one break of the band is picked by `baseCommonality`, among those that can happen now: a binge
needs food it can reach, hiding needs a bed or a roofed cell, a tantrum something to smash, an
insulting spree another colonist, giving up an edge it can reach, and a break whose `requires` names
a feature nothing provides (`FireSpree` needs `"fire"`, E4 calls `mentalBreaks().provide("fire")`)
never starts. The random draws come from the sim's own stream, so the same seed and commands give
the same breaks.

The break's length is picked from `durationHours`. It ends when that time is up **and** the mood
is back above the band's threshold plus `recoveryMoodMargin` (10 points), or at a hard stop of twice
its longest length (at least 12 hours). Binges, sprees and giving up end themselves.

When a break ends calm (or because the colonist was downed) the colonist gets the **Catharsis**
memory (+40 for 2.5 days, `trigger` `MentalBreakEnded`), which is what lifts it clear of the
threshold. Not when ended by the dev command, and not for dying or leaving.

## What each break does

| Break | Band | Does |
| --- | --- | --- |
| `WanderSad`, `WanderSadLong` | minor, major | Picks a reachable cell within 15 m, walks there, stands 4 to 15 seconds, again |
| `FoodBinge` | minor | Goes to the nearest food nobody has claimed, stands 8 seconds over it, eats one helping (past full: the stomach caps at 1, the food is spent); 4 helpings, then ends. No food, no binge |
| `HideInRoom` | minor | Walks to its own bed, else the nearest roofed reachable cell within 40 m, and stays |
| `Tantrum` | major | Walks to the nearest thing within 15 m (items and furniture, **not** walls, doors or building plans) and hits it every 2 seconds for 8 hit points; after 3 things destroyed it is over |
| `InsultingSpree` | major | Walks up to the nearest colonist it hasn't insulted, insults them (`PawnInsulted`), and on to the next; 3, then over |
| `Berserk` | extreme | Goes for the nearest living pawn within 25 m (anyone, any side), else something to smash, and fights it with F8's melee: walks up until a swing reaches, then swings with the weapon in its hands or its fists (real wind-up, dodges, blocks and armour). Things that aren't pawns are still hit for 8 hit points every 2 seconds. Without the melee system it punches every 2 seconds (6 blunt damage) |
| `GiveUp` | extreme | Walks to the nearest reachable edge of the map and leaves for good with what it carries, wears and holds; its bed is freed. `PawnLeftColony`, then the pawn is destroyed |
| `FireSpree` | extreme | Picks one of the five nearest cells within 20 m that would burn and aren't alight, walks beside it, lights it (`Fire::ignite`), waits 5 seconds, and goes on. A burning pawn runs about (`panicStep`) as in the job system. Needs fire: `Sim` calls `setFire`, which provides the `fire` feature |

All the 15 m, 25 m, 2 second and damage numbers are ours (`MentalBreakBehaviour`); RimWorld's break
numbers (rates, curve, lengths) are the D9 data. The tantrum's three-thing limit and the placeholder
fists (kept only for things that aren't pawns) are the two choices to revisit.

## Orders and other systems

- `DraftCommand` (to draft), `MoveOrder`, `AttackOrder`, `OrderHaul`, `ControlPawn` and hands-on
  `Work` are refused for a colonist in a break ("that colonist is in a mental break"). A drafted
  colonist who snaps is undrafted first; its job is interrupted, what it carried put down.
- The protagonist while the player walks it (`PlayerControl`) never snaps.
- A thing destroyed under a pawn's target is forgotten (`forget`); a target that goes away by itself
  (someone ate the food) is simply dropped and a new one found.

## What Unreal sees

- **Snapshot:** `ThingView::mentalBreak` is the `MentalBreaks` def while the pawn is in a break
  (invalid otherwise). Mood and `moodBand` as before.
- **Events:** `PawnMentalBreak` (`message` the break's defName, `amount` the mood), `PawnMentalBreakEnded`
  (`message` `recovered`, `downed`, `died` or `stopped`; `amount` seconds it lasted), `PawnInsulted`
  (`thing` insulter, `target`), `PawnLeftColony`, and `PawnAction` `Punch` (`target` the pawn or thing hit).
  A binge sends `PawnAte` with `message` `binge`.
- **Commands (dev):** `StartMentalBreak{pawn, breakDef}`, `EndMentalBreak{pawn}` ([bridge.md](bridge.md)).
- **Read:** `sim.mentalBreaks().active(pawn)`, `breakOf(pawn)`, `rules()`.

## Saved

Section `mentalBreaks` version 1: `mentalBreaker` per pawn that has had a break (`MentalBreaker`:
whether active, break by defName, state, times, mood to recover to, last end, walk and path ticket,
goal, target, counters, visited targets). A save from before has none and loads fine. Hashed in
`Sim::stateHash`. A break whose def the data no longer has just ends when the save loads.

## Not here

- Social interactions beyond the insult event: the opinion drop and the "insulted" bond effects are C20
  (it can turn `PawnInsulted` into a real interaction); the target already gets the `Insulted` memory.
- The other breaks of RimWorld's DLCs, inspirations,
  and drug-induced states.
- Animations and sounds for the break states; U-tasks read the events and the snapshot.
- Other colonists reacting to a berserker (the defence plan, F6, treats everyone of the player's side
  as a friend).
