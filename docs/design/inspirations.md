# Inspirations

Code: `ColonySim/Inspirations.h`, `Private/Inspirations.cpp`. Tests: `Tests/InspirationTests.cpp`. Data:
`data/defs/Inspirations`, `Rules/InspirationRules.json`. Task C25. Builds on [mood.md](mood.md) and [crafting.md](crafting.md).

A colonist in a good mood is sometimes **inspired**: for about two and a half days one kind of work goes far better.
`InspirationSystem` rolls them, keeps the pawn's `Inspired` component (def, start, end), and the systems that apply an
effect ask it. A pawn has one at a time. Pawns without a `Colonist` record or a mood, prisoners, and downed or
dead pawns are never inspired; a pawn in a mental break is not rolled for.

## When

Every 2,880 game seconds (RimWorld's 2,000 ticks, at a second that depends on the pawn) a colonist whose mood is at least 50
rolls once. The mean wait comes from its mood by a line: 210 days at mood 50, 15 days at mood 100
(`meanDaysByMood`); the chance of the roll is `1 - exp(-interval / mean)`. The rolls come from the second and the
pawn, so the order pawns are walked in cannot change them. If it hits, one of the inspirations the pawn could use is
picked by `baseCommonality`. An inspiration is open to a pawn unless every skill in its `anySkill` is disabled for it.
It ends when its days are up (`expired`), when the thing it is for is done (`used`), or when the pawn goes down.
Numbers are RimWorld's as recalled; all are listed in the defs' `unverified`.

## What each one does

| Inspiration | Needs a skill of | Effect | Used up by |
| --- | --- | --- | --- |
| `InspiredCreativity` | Crafting or Artistic | the next thing made that has a quality is **2 levels better** than its roll (a masterwork roll is legendary; this is the only way to legendary) | that craft (`CraftingSystem::finishProduct(..., qualityShift)`) |
| `InspiredSurgery` | Medicine | the next tend **keeps its medicine** (we have no surgery; RimWorld's cannot fail and uses nothing up); the item is dropped where the doctor stands | that tend |
| `InspiredRecruitment` | Social | the warden's next recruit attempt has **twice the chance** (never over 100%) | that attempt (`PrisonerSystem::interact`) |
| `InspiredTrade` | Social | `statFactors.tradePrice` 0.85 while it lasts; trading reads it with `InspirationSystem::statFactor(pawn, "tradePrice")` when it exists | its days |
| `FrenzyShoot` | Shooting | `aimSpread` x 0.7 and `fireCooldown` x 0.67 while it lasts (read in `CombatSystem`) | its days |

Not done: RimWorld's work frenzy and the other inspirations. A def needs only its fields and, if it has a new
effect, a call from the system that applies it.

## What Unreal sees

- **Events:** `InspirationStarted` (`thing`, `message` the def, `amount` its days) and `InspirationEnded`
  (`message` is `why:def`, `amount` how many game seconds it lasted).
- **Read:** `sim.inspirations().defOf(pawn)`, `statFactor`.
- **Save:** section `inspirations` 1 (`inspired` on the pawn).
- **Tests and tools:** `sim.inspirations().setNatural(false)` stops the rolls; `start(pawn, def, error)` gives one.
