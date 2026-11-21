# Trait effects

Code: `ColonySim/Traits.h` (`TraitStats`, `TraitBook`), `Private/Traits.cpp`, and a line in each system that uses a stat.
Defs: `data/defs/Traits/Traits.json` (the wiki's numbers), thoughts in `Thoughts.json` and `Thoughts3.json`.
Tests: `TraitEffectsTests`, `ColonistTraitNeedsTests`, `MoodTests`, `SocialTests`.

A trait degree lists its numbers in `statOffsets` and `statFactors`. `TraitBook::of(pawn)` folds them into one
`TraitStats` record per colonist (kept after the first ask; not saved), and the sim's systems read it.

| Trait | Effect | Where |
| --- | --- | --- |
| Industriousness (slothful -35%, lazy -20%, hard worker +20%, industrious +35%), Neurotic (+20%, +40%) | every work speed, and research | `JobSystem::lightWorkFactor`, `ResearchSystem::speedAt` |
| Fast walker, jogger, slowpoke | walking speed (`SpeedOffset`, metres per step) | `MovementSystem` |
| Careful shooter / trigger-happy | +5 / -5 shooting skill levels for the aim; aiming takes 1.25x / 0.5x as long | `CombatSystem::shootingSkill`, `aim` start |
| Tough | 30% less damage on every hit (sneed 2026-11-21; RimWorld halves it) | `HealthSystem::hitPart` |
| Wimp | hurt by an enemy in a fight: falls back to cover for 12-20 real seconds (sneed, `fleeWhenHurtSeconds`); melee blows do 10% less damage (sneed, `meleeDamageFactor` 0.9); pain level that downs the pawn -0.5 (only bodies without zones: people have body zones, body-health.md) | `FighterSystem::hurt`, `CombatSystem::traitMeleeDamage`, `HealthSystem::derive` |
| Fast / slow learner, Too smart | experience learned x1.75 / x0.25 | `JobSystem::learnSkill` |
| Great memory | high skills lose half as much a day | `skillDayPassed` |
| Brawler | -10 mood while a gun is in the hands (`BrawlerRangedWeapon`) | situation `wieldsRanged` |
| Pyromaniac | +2 mood per nearby fire up to four, a burning cell counts as two (`PyromaniacBeautifulFire`, within 8 m); its only extreme mental break is the fire-starting spree; cannot fight fires | situation `nearFire`, `MentalBreakSystem::natural` |
| Ascetic | no mood from tables or meal quality (`notTrait` on the four meal memories); bedroom +5 awful, +4 dull, +3 mediocre, 0 decent and slightly impressive, -5 somewhat impressive and better (`AsceticBedroom` adds the difference to `RoomImpressive`) | `Mood.cpp`, `Thoughts3.json` |

Already done before: Gourmand, Quick sleeper, Night owl and Greedy ([needs.md](needs.md), [mood.md](mood.md)); mood offsets
(optimist, sanguine...), break thresholds (nervous, iron-willed, neurotic, too smart); Brawler and Nimble melee numbers;
Kind and Abrasive opinions ([social.md](social.md)); Nudist and Cannibal moods.

Ours (sneed's G2 playtest): a **Nudist** is comfortable 10 C colder (`statOffsets.comfyTemperatureMin` -10,
`TraitStats::comfyMinOffset`, added to the comfort range with the clothes' insulation), so cold nights don't bother
them; the hot end is unchanged. RimWorld's nudist has no temperature change.

## Not yet

- Bloodlust's mood from kills and witnessed deaths: needs the killer in `PawnDied` events.
- Jealous (no def yet): -8 for not having the best bedroom.
- Pyromaniac's start-fires mental break at any mood (MTB 50 days), lit campfires counting for the beautiful-fire thought.
- Gourmand's food binge as the only minor break; Psychopath, Beauty's room effects, Great memory beyond skills.

The protagonist's mood still never takes control (mood only shows), so a Brawler player character just sees the number.
Careful and trigger-happy aim: the wiki's `ShootingAccuracyPawn` +5/-5 is treated as five skill levels (unverified).

## Trait points (ours, sneed 2026-11-21)

Scenarios with `traitPoints` (Crash Landed: 10) make people without backstories: each trait degree has `points` (what it
costs; drawbacks are negative and give points) and a plain `description`. Starting companions buy traits on the new-game
screen (`SetColonistTraitsCommand`, `traitSetFits`: no conflicts, at most 3 drawbacks and 8 traits, spend no more than
the pool plus what drawbacks gave). Everyone else made in that game (joiners, raiders, traders' people) gets random
traits with all points spent (`ColonistGenOptions::traitPoints`, `colonistBuyTraits`). Costs: tough 5, industrious 5,
fast walker 1, jogger 3, skill traits 3, wimp -3, pyromaniac -4 and so on (the json has them all).

**Skill traits** (`pointsOnly`, never drawn for backstory people): natural marksman (shooting), born fighter, builder's
hands, tunnel rat, kitchen-raised, green thumb, animal whisperer, tinkerer, artist's eye, field medic, silver tongue,
bookworm: +3 to one skill, kept as `SkillRecord::bonus`, which also lifts that skill's top by 3 (23). Levels past 20 cost
and decay like level 20; shooting and melee keep improving past 20 (accuracy adds the 19-to-20 step per level, capped at
99.5%; melee lines run on). Drawbacks made from backstory "incapable of": **pacifist** (no violence, -4), **pampered**
(no hauling or cleaning, -3), **clumsy** (-4 construction and crafting, -2), **frail** (20% more damage, -3).

Without backstories, a point-trait person's skills come from the random curve for every skill, stretched by age, with 3
random skills lifted +3 to +6 (`PawnGenRules.pointSkillBoosts`) in place of the backstories' gains.
