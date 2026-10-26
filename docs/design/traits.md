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
| Tough | half the damage on every hit | `HealthSystem::hitPart` |
| Wimp | pain level that downs the pawn -0.5 | `HealthSystem::derive` |
| Fast / slow learner, Too smart | experience learned x1.75 / x0.25 | `JobSystem::learnSkill` |
| Great memory | high skills lose half as much a day | `skillDayPassed` |
| Brawler | -10 mood while a gun is in the hands (`BrawlerRangedWeapon`) | situation `wieldsRanged` |
| Pyromaniac | +2 mood per nearby fire up to four, a burning cell counts as two (`PyromaniacBeautifulFire`, within 8 m); its only extreme mental break is the fire-starting spree; cannot fight fires | situation `nearFire`, `MentalBreakSystem::natural` |
| Ascetic | no mood from tables or meal quality (`notTrait` on the four meal memories); bedroom +5 awful, +4 dull, +3 mediocre, 0 decent and slightly impressive, -5 somewhat impressive and better (`AsceticBedroom` adds the difference to `RoomImpressive`) | `Mood.cpp`, `Thoughts3.json` |
| Bloodlust | +12, +24, then +37 mood (`KilledBloodlust1` to `3`, 4 days; a kill moves it one stage up) for killing a person. `PawnDied` names the killer in `target`: whoever last hurt the victim (`Health::lastAttacker`, set by shots, melee blows and punches), when it died of its wounds | `MoodSystem::noteKills`, `HealthSystem::die` |
| Jealous | -8 (`JealousBedroom`) while another colonist's bedroom is more impressive than its own, or it has none while another has one | situation `worseBedroom` in `Sim.cpp` |
| Gourmand | its only minor break is the food binge (no food: no minor break) | `MentalBreakSystem::natural` |
| Pyromaniac (break) | the fire-starting spree also comes at any mood, about once in 50 days (`pyromaniacMeanDays`) | `MentalBreakSystem::natural` |

Already done before: Gourmand, Quick sleeper, Night owl and Greedy ([needs.md](needs.md), [mood.md](mood.md)); mood offsets
(optimist, sanguine...), break thresholds (nervous, iron-willed, neurotic, too smart); Brawler and Nimble melee numbers;
Kind and Abrasive opinions ([social.md](social.md)); Nudist and Cannibal moods.

## Not yet

- Bloodlust's mood from witnessed deaths; explosions and traps do not name a killer yet (a blast or a spike trap kills with no `target`).
- Lit campfires counting for the pyromaniac's beautiful-fire thought.
- Psychopath, Beauty's room effects, Great memory beyond skills.

The protagonist's mood still never takes control (mood only shows), so a Brawler player character just sees the number.
Careful and trigger-happy aim: the wiki's `ShootingAccuracyPawn` +5/-5 is treated as five skill levels (unverified).
