# Skill learning

Code: `ColonySim/Skills.h`, `Private/Skills.cpp` (the maths), `Private/GatherJobs.cpp`
(`JobSystem::learnSkill`, `skillsNewDay`, `activeSkill`), `Private/Mood.cpp` (the passion thought).
Tests: `Tests/SkillsTests.cpp`, and the colony scenario's `skillXpGained`. Data: `SkillRules` and
`WorkRules` in `data/defs/Rules`, the `PassionMinorWork` and `PassionMajorWork` thoughts. Task C12.
Builds on [colonists.md](colonists.md) (skills, passions) and [mood.md](mood.md).

A colonist gets experience (XP) in a skill from every job that uses it, a level at a time. This
page is how that works and how a later task makes its own job teach.

## Where XP comes from

`JobSystem::learnSkill(pawn, skill, xp)` is called **once for every second a job works**, by the job
itself. It does nothing for a pawn with no `Colonist` record (a test pawn) or without that skill.

| Job | Skill | XP a second | Source |
| --- | --- | --- | --- |
| Mine | Mining | 0.0486 | RimWorld 0.07 a tick (C8), `WorkRules.mining.xpPerSecond` |
| Cut or harvest a plant | Plants | 0.0590 | RimWorld 0.085 a tick (C8), `WorkRules.plants.xpPerSecond` |
| Build a frame, take a building down | Construction | 0.1736 | RimWorld 0.25 a tick, `WorkRules.construction.xpPerSecond`, `unverified` |
| A bill (cooking now; crafting, tailoring, surgery later) | the recipe's `workSkill` | `workSkillXp / work` | the recipe, see below |

RimWorld ticks are 1.44 game seconds, so every per-tick number is divided by 1.44.

**Bills.** A recipe's `workSkillXp` is the XP a whole run gives *at 100% work speed*, spread evenly
over the run's `work` seconds. The XP does not follow the pawn's speed, as in RimWorld (XP per tick,
not per unit of work): a slow cook takes longer at the same rate, so learns more from one meal.
Every recipe with a `workSkill` teaches, so a D task that adds recipes only has to write the number.

Hauling, eating, sleeping, walking and the like use no skill and teach nothing. **A new job that
uses a skill calls `learnSkill` each working second and lists itself in `JobSystem::activeSkill`**
(so the passion mood works for it); a sowing, hunting or doctoring task does both.

## What a second of work is worth (`learnSkillXp`)

1. The passion's factor: none 0.35, minor 1.0, major 1.5 (`SkillRules.passionLearnFactor`).
2. **Daily soft cap.** Once a skill has earned `dailyXpSoftCap` (4,000) XP since midnight, what it
   learns after that is multiplied by `xpFactorOverCap` (0.2). The day's total is
   `SkillRecord::xpToday`, counted after the passion factor; the step that crosses the cap still
   counts in full. The numbers are the wiki's; exactly what the cap counts is `unverified`.
3. The XP is added; while it is at least `xpToNextLevel[level]` the level goes up and that much is
   taken off. A **disabled** skill (backstory or trait) learns nothing.
4. At level 20 the XP stops at `xpToNextLevel[20] - 1`, so a master can keep hold of the skill
   against the decay.

## Midnight (`skillDayPassed`, from `Sim::step` when the day ends)

- `xpToday` goes back to 0.
- From level 10 up the skill loses `decayXpPerDay[level]` XP (30 at level 10, 120 at 12, 300 at 14,
  1,200 at 17, 3,600 at 20). XP may sink `xpBelowZeroBeforeLevelLoss` (1,000) below 0 before the
  level drops; the level below then gets its `xpToNextLevel` added, so nothing jumps. Level 0
  never goes below 0 XP. Passion does not change the decay (`unverified`: RimWorld may soften it).
- A level-20 skill needs 3,600 XP a day to hold; the cap (4,000) lets a busy master do it.

## Mood: passion at work

While a colonist's job uses a skill it has a passion for, it gets a situation thought: **doing work
I enjoy** (minor, +8) or **doing work I love** (major, +14); the size is `SkillRules.passionMood`.
It lasts for the whole job, the walk to the work included, and is gone when the job ends (the mood
then eases back at the usual 8 an hour). `JobSystem::activeSkill(pawn)` says which skill the job
uses (empty for none); `MoodSystem` is given it with `setActiveSkill`. The colonist panel shows the
thought like any other (`MoodSystem::thoughts`).

## Saving

`SkillRecord::xpToday` is new: `colonists` section version 2 (older files load with 0). It is in
`Sim::stateHash`.

## Not yet

- Direct versus indirect learning, and skills that learn from being near work (RimWorld's
  `Learn(xp, direct)`); inspirations; skill gain from reading or training dummies.
- Passions changing or being gained (RimWorld has no way to in vanilla besides events).
- Showing XP in the colonist panel (U8 shows levels and passion now; the XP bar comes with the styled UI).
- Jobs that do not exist yet teach nothing until their task adds the line: sowing, hunting,
  butchering, taming, training, doctoring, repairing, cleaning, researching, social and the rest.
