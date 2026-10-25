# Mood and thoughts

Code: `ColonySim/Mood.h`, `Private/Mood.cpp`. Tests: `Tests/MoodTests.cpp`, `simrun --start Crashlanded
--days 3 --check` (`simrun_needs`, prints a `mood` line). Data: `data/defs/Thoughts`, `MoodRules` in
`data/defs/Rules`. Task C9. Builds on [needs.md](needs.md) and [colonists.md](colonists.md).

A colonist with needs also has a **mood**, 0 to 100 (`Mood::level`). It is made when the pawn first
steps (so no change to how pawns are spawned) and starts at the mood it is heading for. Pawns
without needs, or data without a `MoodRules` def, have none.

## Where mood is heading

`MoodSystem::target` is the **base mood** (`MoodRules.baseMood`, 50) plus the trait mood offsets
(`moodOffset` of each trait degree: optimist +6, depressive -12...) plus every **thought** that counts
now. The level moves towards the target (clamped to 0 to 100) at most **12 an hour up and 8 an hour
down**, as the wiki says, and **stands still while the pawn sleeps**.

Base mood 50 is ours: the wiki's base is by difficulty (32 on Strive to Survive) but RimWorld's colonists
sit higher because comfort, beauty, outdoors, joy and the rest add to it, and we haven't built those. Revisit
when they exist (`ours` in the def).

## Thoughts (`Thoughts` defs)

Two kinds. Numbers are RimWorld's as recalled; only Hungry (-6) is from the wiki, the rest are
listed in each def's `unverified`.

| Thought | Mood | Kind | When |
| --- | --- | --- | --- |
| Hungry / Ravenous / Starving | -6 / -12 / -20 | situation | food band Hungry, Ravenous, Malnourished |
| Drowsy / Tired / Exhausted | -6 / -12 / -18 | situation | rest band Drowsy, Tired, Exhausted |
| Ate without a table | -3 | memory, 0.6 days | finished a meal (any meal kind) standing, not at a chair and table |
| Ate raw food | -7 | memory, 0.6 days | food whose `ingestible.preferability` is `RawBad` (raw meat); berries (`RawTasty`) are fine |
| Ate a fine meal | +5 | memory, 0.6 days | `MealFine` |
| Ate a lavish meal | +12 | memory, 0.6 days | `MealLavish` |
| Slept on the ground | -4 | memory, 0.6 days | woke up from sleeping with no bed |
| Slept outside | -6 | memory, 0.6 days | woke up in a cell that is not indoors (`Rooms::indoors`: under a roof and not in an outdoor room, [rooms.md](rooms.md)); a bed outside gives this one only, the bare ground both |

A **situation** exists while the state holds and is not stored. A **memory** is added from the step's
`PawnAte` and `PawnWokeUp` events, lasts `durationDays` (`MoodRules.memoryDays`, 0.6, if the def has none)
and a second one of the same thought starts the fading over (one of each). A thought def is a situation
if it has `situation: {need, band}` (need `food` or `rest`, band as the need's band names in
needs.md), a memory if it has `trigger` (the names the table says). The trigger rules live in
`MoodSystem::step`; a new memory needs its trigger there and a def.

`MoodSystem::thoughts(pawn, now)` lists what counts now for the colonist panel (U8): label, offset, whether
it is a situation, when a memory fades; strongest first.

## Mental break thresholds (recorded, no breaks yet)

`MoodBand`: Stable, Minor, Major, Extreme (break risk). The mood is **under** a threshold's level
(35%, 20%, 5% of the scale, `MoodRules.breakThreshold`) when in that band; the worst band wins. A trait's
`statOffsets.mentalBreakThreshold` (Nerves: volatile +0.15, iron-willed -0.18) moves all three
(`MoodSystem::breakLevels`). `Mood::band` is kept up to date; when it changes a **`PawnMoodBand`** event
says so (`message` the new band, `amount` the mood). Nothing happens yet: mental breaks come later.

## Passion at work

Two more situations, `PassionMinorWork` (+8) and `PassionMajorWork` (+14, from `SkillRules.passionMood`),
count while the pawn's job uses a skill it has that passion for ([skills.md](skills.md)). A situation def
has `situation: {passion: "Minor"}` instead of a need and band.

## What Unreal sees

- **Snapshot:** a pawn with a mood has `mood` (else -1) and `moodBand` (0 to 3).
- **Events:** `PawnMoodBand`.
- **Read:** `sim.mood().thoughts(pawn, seconds)`, `target`, `breakLevels`.

## Save format 11

`Mood` (level, band, memories by thought `defName` and fade time) is saved per pawn; formats 6 to 10
still load (the mood starts fresh). A memory whose def the data no longer has stays in the file and counts
for nothing.

## Not yet

Mental breaks, inspirations, ideology and precepts; thoughts from pain, injuries, rooms, beauty,
temperature, joy, relationships, other pawns' deaths; social and mood stacking beyond one of each; thoughts from traits' `moodWithRangedWeapon`.
