# Team play: allies fight together without fixed squads

sneed, 2026-11-18: "it should just be ai that responds to nearby allies rather than a fixed squad so they adapt and are
situationally aware (pawns allies always know where the others are so they dont have to see them), ... smart but also
aggressive but also varied a bit for unpredictability." Earlier the same night: ranged pawns act like Crysis enemies
(cover, call-outs, swap weapons, back off from melee and shoot); the focus/lock-on is mostly for melee fighters.

Sim only (`Private/FighterTeam.cpp`, hooks in `Fighters.cpp`), numbers in `data/defs/Rules/FighterRules.json` (all ours:
RimWorld has no team AI).

## Game references

- **F.E.A.R. (2005)**: no real squad brain. Each soldier plans alone, but reads what the others are doing and
  shouts it ("flanking!", "cover me!"), so the player reads it as teamwork. This is the model we copy.
- **Halo 2/3**: enemies get bolder when they win and break when their leader or friends drop; brave and cowardly kinds.
- **The Last of Us Part II**: enemies push when you go quiet (reloading) and call out losses by name.
- **Crysis**: cover, suppress and flank on open maps; the ranged behaviour sneed asked for.

## How it works

No squads. Every plan (twice a second) each fighter looks at the friends of its side within `allyRadius` (30 m). It
always knows where they are and what they are doing (target, part, whether they have a shot): this is not stealth, which
is about enemies. It then picks its own **part**:

| Part | When | What it changes |
|---|---|---|
| **FallBack** | the old rules (outnumbered, badly hurt, a Wimp hurt, the raid routs) **or shaken**: friends near it went down lately (1 + 3 x aggression of them within 15 s), unless its side is overwhelming | as before: cover counts double, it backs away |
| **Push** | its target is badly hurt, backing off or fleeing, or its gun is empty with enough cooldown left to run half the gap first (the "reload" of other games), or (melee) a friend is already at it; taken with a chance of 0.3 + 0.65 x aggression (less when pinned down) | a gunman wants each metre nearer (`pushWeight`), minds being seen half as much and comes to 2 m; it sprints there. A melee fighter stops waiting with the others and charges |
| **Suppress** | nobody near is shooting its target | it wants a spot with a shot even more and keeps it |
| **Flank** | a friend is pinning the target, fewer than `maxFlankers` (2) are going round already; chance 0.2 + 0.6 x aggression | spots `flankDegrees` (70) or more round from the friends shooting, on its chosen side (the other way from a friend already flanking), and where the target's own cover doesn't face |
| None | otherwise | the old scoring: hold, shoot, focus fire |

A part is kept at least `roleHoldSteps` (3 s) while it still makes sense, so fighters don't flicker between them.

**Suppression.** An enemy shot passing within 1.5 m keeps a fighter's head down for 2 s: cover counts up to 1.6 times
more in its plans (the bold mind it less) and it is less likely to push or flank.

**Melee surround.** A melee fighter charging a target a friend is already at runs to the target's far side, so they
come at it from two sides.

**Aggression** (0 to 1): colonists 0.45, raiders 0.6, plus or minus up to 0.15 rolled once per pawn from the world seed,
plus 0.015 per level of its best fighting skill over 10, plus traits (Brawler +0.15, Bloodlust +0.1, Psychopath +0.05,
Tough +0.1, Wimp -0.3). Each plan also rolls its own dice from the seed and the step, so two alike fighters don't
always do the same thing. All of it replays the same.

**Call-outs.** On taking a part (and when a friend or an enemy goes down) a fighter shouts: a `PawnAction` `Callout`
event with `message` "suppressing", "flanking left/right", "pushing", "falling back", "man down" or "enemy down" and
`amount` the `Callout` number. One fighter calls at most every 5 s, and a call a friend within 15 m just made isn't
repeated. Unreal does nothing with them yet: voice barks are a PC job.

A fighter under the player's own move or attack order holds as told: it never pushes or flanks.

The existing rules all still apply: fleeing and flee responses, taking cover when shot, weapon swapping by distance,
standing and fighting with an enemy at arm's length, raid retreats, focus fire, stealth.

## Off switch

`"teamPlay": false` in `data/defs/Rules/FighterRules.json` turns all of it off: no parts, suppression, shaking,
surrounding or call-outs. Fighters then behave exactly as before team play (test "switched off in FighterRules").

## Saves, hash, tests

Fighter fields `role`, `roleSince`, `flankSide`, `lastCall`, `calloutAt`, `alliesDown`, `allyDownAt`, `suppressedUntil`
(save section `fighters` 6; hashed only away from their defaults). Tests `TeamPlayTests.cpp`.

## Not yet

- Unreal: voice barks for call-outs, a hand signal when flanking.
- Ranged backpedal from melee and melee retargeting: the lock-on thread (`docs/design/lock-on.md`).
- Grenades to flush a target out of cover once flanking fails; a "last stand" push when cornered beyond the raid rule.
