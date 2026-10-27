# Research

Code: `ColonySim/Research.h`, `Private/Research.cpp` (the system), `Private/ResearchJobs.cpp` (the job),
`ResearchGiver` in `Private/WorkGivers.cpp`. Tests: `Tests/ResearchTests.cpp`. Task C21. Data: D12
([research-data.md](research-data.md)); numbers in `data/defs/Rules/ResearchRules.json`.

The colony has **one current project**. A colonist with the Research work type (Intellectual skill) goes
to a research bench that can work on it, sits beside it and adds points every game second. At the
project's cost the project is done, the colony has no current project any more (as RimWorld), and a
`ResearchDone` event is raised. Progress of a project that was put down is kept.

## Commands

| Command | What it does |
| --- | --- |
| `SetResearchCommand{project}` | Picks the project (a `Research` defName); an empty name puts the current one down. Fails for an unknown project, a finished one, or one whose prerequisites (shown or hidden) are not all done. |
| `UnlockResearchCommand{project}` | Dev: the project is done at once, whatever it needs. Raises `ResearchDone` with no pawn. |

## Gating

`PlaceBlueprintCommand` and `AddBillCommand` fail with "<label> needs the research <Project>" when the Things def
(or the recipe) names a `researchPrerequisite` that is not done (`ResearchSystem::unlocked(def)`, which
reads the one name or the list; the parent's value is inherited by the defs data). Defs without the field,
and games whose data has no Research defs (the test defs), are never gated. Dev spawns
(`SpawnThingCommand`) and a bill already on a bench are not checked: research only ever opens things up.
A new colony starts with nothing done; a Scenarios def may list `startingResearch` (project names, done when
the scenario starts) and `neolithicStart` (true: projects cost the tech level's `costFactorForNeolithicStart`,
1.5 for medieval, 2 for industrial and up; `ResearchSystem::cost`). Crashlanded has neither.
Not gated yet: terrain (floors) and plants, because nothing places them by command; whoever adds that calls
`research().unlocked(def, error)` the same way.

## The job

`JobType::Research` (steps: go to the cell beside the bench, then work). The work giver (`Research` task of the
Research work type) offers every finished research bench that is not marked for deconstruction, has power
(`JobSystem::buildingRuns`) and whose `researchBench.tier` is at least the current project's `benchTier`, and
that the pawn can walk up to. One pawn per bench (the bench is reserved). Each second:

```
points = pointsPerTick / secondsPerTick          (0.00825 / 1.44 = 0.00573 a second at 100%)
       x (skillSpeedBase + skillSpeedPerLevel x Intellectual level)    (0.08 + 0.115 x level: 8% at 0, 100% at 8, 238% at 20)
       x bench speedFactor                        (simple 0.75, hi-tech 1.0)
       x Consciousness
```

A colonist with no skills counts as level 8. The job ends, done and not failed, when the project is finished,
put down or needs a better bench; it fails (and the pawn avoids that bench a while) if the bench is gone,
unpowered or the pawn can't think. A `PawnAction::Research` event (target: the bench) is raised once at the start for
Unreal's animation. Numbers: RimWorld's 0.00825 per tick is from the wiki; the skill curve is ours (a
`ResearchRules` entry says so) until checked.

Not done: the multi-analyzer's +0.1 speed and the `requiredFacility` check (the facility isn't defined yet; a
project that names one is treated as a plain tier 2 project), skill XP from research (no skill XP exists in the
sim yet), research while hands-on in direct mode, research speed from light or recreation.

## Snapshot and save

`Snapshot::research` lists every project in def order: `project`, `progress`, `cost`, `done`, `available`
(not done, all prerequisites done), `current`. U18's research tab reads it and sends `SetResearchCommand`.
Save section `research` (version 1): current project, neolithic flag, finished projects (sorted), progress
as two parallel lists. Projects the data no longer has are dropped on loading. Hashed in `Sim::stateHash`.
