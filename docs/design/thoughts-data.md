# Thoughts, recreation, mental break and social data

Task D9. Data only: no sim code changed. The tasks that use each file are named below. All numbers
are RimWorld's as recalled (`unverified` in each def); check against the wiki when a task leans on
one. Folders under `data/defs/`.

## Thoughts (`Thoughts/Thoughts2.json`)

Same two kinds as [mood.md](mood.md). New fields:

- **Situation with stages.** `situation.condition` names a state the owning task evaluates
  (`coldBelowComfort`, `noApparel`, `roomBeauty`, `painLevel`...); `stages` is a list of
  `{label, moodOffset}`, worst first, and the def's `...Thresholds` list says where one stage ends
  and the next begins (the owning task decides what the number measures; the `note` says). The mood
  system reads only `situation.need` food or rest today, so these do nothing until their task.
- **Memories** use `trigger`, `moodOffset`, `durationDays` as before. A trigger is an event name the
  owning task raises (`KnowColonistDied`, `MentalBreakEnded`...).

Covered: temperature (E2), clothes and nudity (C15), beauty, space, impressiveness, cleanliness,
barracks (C17), comfort, outdoors, recreation (C18), darkness (E3), pain, sickness, deaths and
corpses, social memories and bond deaths (C20), catharsis (+40 for 2.5 days after a break, C19).
Nudity is thoughts only, no sexual content.

## Recreation

`RecreationKinds`: meditative, social, dexterity, cerebral, each with tolerance gain per use and
decay per day. `RecreationActivities`: horseshoes, chess, billiards, walk, watch the clouds, chat at
a table; each names a kind, optionally a `building` (or `buildingFlag` for any table), `users`
[min, max], `joyPerHour`, `durationHours` [min, max], `outdoors`. `JoyRules` holds the fall rate,
joy hours and the tolerance formula. The buildings are in `Things/Buildings.json`
(`building.joyKind`, `maxSimultaneousUsers`, `standCells` offsets).

## Mental breaks

`MentalBreaks`: minor (sad wander, food binge, hide in room), major (tantrum, long sad wander,
insulting spree), extreme (berserk, give up and leave, fire-starting spree). `band` matches
`MoodBand`, `baseCommonality` is the weight among the band's breaks, `state` is the name the C19
job code uses. `MentalBreakRules` has the mean days between breaks per band and the mood curve.

## Social

`Interactions` (chitchat, deep talk, kind words, slight, insult, romance attempt, marriage proposal,
breakup) give opinion changes (`recipientOpinion`), thoughts, and for the last three a `bond`.
`Bonds`: friend and rival are *derived* from opinion (`opinionAtLeast` / `opinionAtMost`); lover,
spouse and the ex bonds are stored. Romance and marriage are bonds only: no sex, no pregnancy.
`SocialRules` has the opinion thresholds, how often pawns talk and the Social XP.

`tools/check_defs.py` checks the new cross-references (activities to kinds and buildings,
interactions to thoughts and bonds).
