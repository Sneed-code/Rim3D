# Double beds and lovin

Code: `Private/Lovin.cpp` (part of `SocialSystem`), bed sharing in `JobSystem::findBed` (`Private/NeedJobs.cpp`), the
`wantToSleepWithPartner` situation in `Sim.cpp`. Tests: `Tests/LovinTests.cpp`, and the save audit has a double bed and
a couple. Data: `DoubleBed` (`Things/Buildings.json`), `SocialRules.lovin` (`Rules/Rules.json`), thoughts `GotSomeLovin`
and `WantToSleepWithPartner` (`Thoughts2.json`). Decision: [0008](../decisions/0008-abstract-lovin.md). It stands on
social life ([social.md](social.md)) and beds ([needs.md](needs.md)).

Partners (lovers or spouses) share a double bed, and now and then have **lovin**, as in RimWorld. It is abstract and
adults only: the two stay lying in their bed under the covers a while, and afterwards each is happier and thinks a
little better of the other.

## The double bed

`DoubleBed`: 2 x 2, two sleeping slots side by side, 85 stuff, 200 HP, work 1100 ticks (1,584 s), beauty 2, comfort
0.75, rest effectiveness 1.0 (RimWorld's numbers, recalled). Unlocked by **Complex furniture**. A room with one double
bed is a bedroom (rooms count beds, not slots).

## Sharing a bed

- A pawn whose partner owns a bed with a free slot moves into it when it next goes to bed, giving up its own bed.
- Two colonists who both have a social life share a bed only if they are partners (RimWorld's rule): a stranger never
  takes the free slot of someone's double bed. Pawns without a social life (no `SocialRules`) share as before.
- `AssignBedCommand` still puts anyone in any free slot: the player decides.
- **Want to sleep with partner** (-3, RimWorld): a partner on the map whom the pawn thinks well of (opinion over 0),
  and no bed in common.

## Lovin

Checked every game second for each couple (`SocialSystem::stepLovin`, after the fights):

- Both lie in the **same bed** on their sleep job, and the later of the two lay down at most `windowMinutes` (30, ours)
  ago. We have no awake-in-bed state; RimWorld asks that the partner be awake, and the window stands for that.
- Both are free for it: alive, on the map, on their feet, **a Colonist record with age at least `adultAge` (18)** (a
  pawn without one never has lovin), not in a mental break, a fist fight or under the player's hand, not asexual (ours),
  and past their own `nextLovin`.

Then both get a `Lovin` component (partner, start, end, pose) for 6 to 66 minutes (RimWorld's 0.1 to 1.1 hours, from a
hash roll). While it lasts each stays lying in bed whatever its rest. It ends early, with no thought, if either gets up
(drafted, the alarm, fire, starving), is downed or taken under the player's hand.

When it ends in time each partner gets:

| Effect | Size | Where from |
| --- | --- | --- |
| Mood thought *got some lovin'* | +10 for 1 day | RimWorld, recalled |
| Opinion memory *lovin'* about the other | +10 for 1 day, one at a time | RimWorld, recalled |
| A pause before the next | RimWorld's curve over age: 1.5 hours to 22, 4 at 30, 12 at 50, 36 at 75, plus a normal spread of 0.3 hours, at least 0.5 | recalled |

The opinion helps marriage proposals (they need 40 both ways) and keeps cheating less likely (it needs the partner
under 60).

**Pose** (`LovinPose`, cosmetic): side by side, spooning or embracing, one per lovin from a hash roll. Unreal lays the
couple under the covers in it and shows a small hearts icon; a kiss now and then is fine. No fade (the game runs in
real time), no nudity in it and no sex animation.

## What Unreal sees

- **Snapshot:** per pawn `lovin` and `lovinPose` (0 side by side, 1 spooning, 2 embracing); `asleep` stays true.
- **Event:** `Lovin` (`thing` and `target` the partners, `message` "started" with `amount` its seconds, "done" or
  "interrupted").
- **Reads:** `sim.social().inLovin(pawn)`, `lovinIntervalHours(pawn)`, `stats().lovins`.

The Unreal side (the hearts icon, the poses, a double-bed mesh) is a PC task in `BACKLOG.md`.

## Save and hash

Section `social` 2: `Social.nextLovin` and the `Lovin` component (older saves load with neither). A pawn that moves to
another map loses its `Lovin`, and an away pawn's pause is not kept (`maps` 1). Everything is in `Sim::stateHash`.

## Not yet

- RimWorld's health and opinion factors on how often (we use age only).
- Pregnancy and children: not in the game.
