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

Another system can start a memory with `MoodSystem::remember(pawn, trigger, now)`: the social system (C20, [social.md](social.md)) does for chats, slights, insults, weddings, breakups and deaths of friends.

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

## Mental break thresholds (the breaks themselves are [mental-breaks.md](mental-breaks.md), C19)

`MoodBand`: Stable, Minor, Major, Extreme (break risk). The mood is **under** a threshold's level
(35%, 20%, 5% of the scale, `MoodRules.breakThreshold`) when in that band; the worst band wins. A trait's
`statOffsets.mentalBreakThreshold` (Nerves: volatile +0.15, iron-willed -0.18) moves all three
(`MoodSystem::breakLevels`). `Mood::band` is kept up to date; when it changes a **`PawnMoodBand`** event
says so (`message` the new band, `amount` the mood). The mental break system (C19) rolls for a break from the band; a
break that ends calm raises the `Catharsis` memory (trigger `MentalBreakEnded`) and an insult the `Insulted` one.

## Temperature thoughts (E2)

A thought def with `situation.condition` `coldBelowComfort` or `hotAboveComfort` (D9's `Cold` and `Hot`)
counts while the pawn is outside its comfortable temperature range; its `stages` are picked by the degrees
outside it ([temperature.md](temperature.md)). The thought's label in the panel is the stage's (`chilly`,
`freezing`, `warm`, `scorching`). Like the food and rest situations it exists only while it holds.
## Trait situations

Three staged thoughts in `Thoughts3.json` are limited to a trait (`situation.trait`) and judged in `Sim.cpp`:
`NightOwlAwakeAtNight` (+8) and `NightOwlAwakeInDay` (-5) count while a Night owl is awake (not lying down), by
`MoodRules.nightHours` (ours, 21:00 to 04:59); `GreedyNoBedroom` (-4) counts while a Greedy colonist owns no bed in a
room with the Bedroom role. The numbers are half the traits' `moodAwakeAtNight`, `moodAwakeInDay` and `moodWithoutBedroom`: the full ones sent the Crashlanded colony into mental breaks (ours, tune in a playtest).

## Passion at work

Two more situations, `PassionMinorWork` (+8) and `PassionMajorWork` (+14, from `SkillRules.passionMood`),
count while the pawn's job uses a skill it has that passion for ([skills.md](skills.md)). A situation def
has `situation: {passion: "Minor"}` instead of a need and band.

## What Unreal sees

- **Snapshot:** a pawn with a mood has `mood` (else -1) and `moodBand` (0 to 3).
- **Events:** `PawnMoodBand`.
- **Read:** `sim.mood().thoughts(pawn, seconds)`, `target`, `breakLevels`.

## Staged situations (C17)

A thought with `condition`, `stageThresholds` and `stages` is one thought whose stage follows a value
from `RoomStats::conditionValue` (beauty need, room space, impressiveness, cleanliness, shared
bedroom). See [room-stats.md](room-stats.md).

## Save format 11

`Mood` (level, band, memories by thought `defName` and fade time) is saved per pawn; formats 6 to 10
still load (the mood starts fresh). A memory whose def the data no longer has stays in the file and counts
for nothing.

## Staged situations (C18)

A thought def with `situation.condition` and `stages` (D9's Thoughts2) is a situation that has several
stages: `MoodSystem::conditionValue` measures a number for the condition (`joyNeed`, `comfortNeed`,
`outdoorsNeed` so far, [recreation.md](recreation.md)), the def's `<condition without Need>Thresholds`
list says where each stage ends, and the stage's `moodOffset` counts. A condition nobody measures yet
(temperature, clothes...) gives nothing. A task that owns a condition adds its case to `conditionValue`.

## Not yet

Mental breaks, inspirations, ideology and precepts; thoughts from pain, injuries,
joy, relationships, other pawns' deaths; social and mood stacking beyond one of each; thoughts from traits' `moodWithRangedWeapon`.
Inspirations, ideology and precepts; thoughts from pain, injuries, rooms, beauty,
temperature, joy, relationships, other pawns' deaths, passions at work (`SkillRules.passionMood` is read by
nothing yet); social and mood stacking beyond one of each; thoughts from traits' `moodWithRangedWeapon`.
