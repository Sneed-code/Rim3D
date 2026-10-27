# Social life

Code: `ColonySim/Social.h`, `Private/Social.cpp`. Tests: `Tests/SocialTests.cpp`, the colony scenario
([colony-scenario.md](colony-scenario.md)) counts talks. Data: `Interactions`, `Bonds`, `SocialRules`
(`Rules/Rules.json`), `opinionOfMe` on traits, the social thoughts ([thoughts-data.md](thoughts-data.md)).
Task C20. It stands on mood ([mood.md](mood.md)), melee ([melee.md](melee.md)) and recreation
([recreation.md](recreation.md)).

Colonists talk. A talk is an **interaction** (chitchat, deep talk, kind words, a slight, an insult, a
romantic gesture, a marriage proposal, a breakup). Each changes what the listener thinks of the talker (an
**opinion**), may leave a mood thought, and gives the talker Social experience. **Friends** and **rivals**
follow from opinions; **lovers**, **spouses** and their **exes** are stored **bonds**. Romance and marriage
are bonds only: no sex, no pregnancy, no children. An insult can start a **fight with fists**.

A pawn takes part if it has needs, is not an animal and is on the player's side. It gets a `Social`
component the first step it exists (`nextTalk`, `memories`, `bonds`, `mourned`).

## Opinion

`SocialSystem::opinion(pawn, other)` is a number from -100 to 100 (`SocialRules.opinionMin/Max`), the sum of:

| Part | Size | Where from |
| --- | --- | --- |
| Compatibility | -10 to 10, the same both ways | a hash of the pair and the world seed (`compatibilityMax`, ours) |
| What the other is like | the other's traits' `opinionOfMe`: Abrasive -20, staggeringly ugly -10, ugly -5, pretty 5, beautiful 10 | unverified |
| Bonds | Lover +40, Spouse +60, ExLover -25, ExSpouse -25 | the `Bonds` defs |
| Memories | what happened between them, each fading after its days | the interactions |

A memory (`OpinionMemory`: who it is about, a label, an offset, when it fades) is kept by the pawn that
holds the opinion. A pawn holds at most `stackLimit` (3) of one label about one other; a fourth takes the
oldest one's place. `opinionLines(pawn, other)` lists the parts for the colonist panel.

**Friend:** both think at least 20 of each other (`friends(a, b)`). **Rival:** `pawn` thinks at most -20 of
`other` (`rival(pawn, other)`; one-sided, so a rival may not know). Neither is stored.

## Interactions at RimWorld's rates

The data is D9's (`Interactions`): weight, the listener's opinion change (`recipientOpinion`: label, offset,
days), mood thoughts for the talker and listener, Social experience, the opinion limits, and for the last
three a bond.

| Interaction | Weight | Listener's opinion | Needs |
| --- | --- | --- | --- |
| Chitchat | 1 | +1, 15 days | |
| Deep talk | 0.075 | +6, 30 days | the talker thinks at least 0 of the listener |
| Kind words | 0.01 | +5, 15 days | the talker thinks at least 10; the listener's mood is under 40 |
| Slight | 0.15 | -5, 30 days | the talker thinks at most 20 |
| Insult | 0.2 | -20, 30 days | the talker thinks at most -10 |
| Romance attempt | 0.1 | +10, 30 days if it works | thinks at least 5 (both ways), adults, both single |
| Marriage proposal | 0.05 | | thinks at least 40 (both ways), lovers |
| Breakup | 0.02 | -30, 60 days | lovers, the talker thinks at most 0 |

**When.** Every game second each pawn that is awake, on its feet and not fighting, drafted or under the player's
hand rolls once. The chance is one in `interactionMeanHours` hours (6, the D9 number), or one in
`tableChatMeanMinutes` (15 minutes, ours) while the pawn is at a recreation building or table with
others (a game, chatting at a table: the `Recreate` job with a building). The roll is a hash of pawn and
second, never the sim's random stream. A pawn that talked, either way, starts no talk for `minGapMinutes` (20).

**Whom.** Another such pawn within `maxDistance` (6 m) **in the same room** (both out in the open counts);
at a table only the ones at the same building. Every interaction that fits, for every candidate, is a pick
weighted by its `selectionWeight` times the talker's trait weights (`SocialRules.traitWeights`: a Kind
pawn's slight and insult weights are 0, an Abrasive one's are 3 times).

**Doing it** (`perform`). The listener's opinion memory is added; mood thoughts start (for the talker and
the listener: a chat, a deep talk, kind words, slighted, insulted); the talker earns Social experience (the
interaction's `socialXp`, scaled by passion like any skill, through `JobSystem::addSkillXp`); an `Interaction`
event is sent (`count` 1 if it worked, 0 if refused).

**Romance, marriage, breakup.** A romance attempt or proposal works with `successBase` (0.3, 0.5)
times `1 + the target's opinion of the asker / 100`, at most 95%. A romance that works makes both **Lover**
(the listener also gets the +10). A proposal that works turns the lover bond into **Spouse** and both get
*got married*; one refused leaves the bond and the asker gets *rejected my proposal*. A breakup makes both
**ExLover** (both get *broke up*, the listener thinks -30 of the talker for 60 days) and a spouse never breaks
up. A pawn in a bond is not asked (but may stray, see below). There are no
orientations yet: anyone may love anyone else (BACKLOG). Adults only (`Colonist.age` of at least 18; pawns
without a Colonist record count as adults).

**Death.** The first second a pawn is dead, those who cared get one thought: the strongest of *my spouse
died* (-20), *my lover died* (-16), *a friend died* (-8) or *my rival died* (+6), from the `Bonds` defs. The
lover and spouse bonds with the dead end, so the widow can love again.

## Fights with fists

An insult starts a fight with `fightChance` (10%, ours; 20% if the listener is a Brawler; never with a Wimp
or if the listener is Kind). Both pawns get a `SocialFight` (opponent, started, ends-by, next move):

- The job ends and the job system leaves them alone while it lasts (as for a `Fighter`).
- Each walks up to the other (every 2 s) and swings with **fists, even with a gun in the hands**
  (`MeleeSystem::attack(..., fists=true)`; reach and tools are the body's). The swing, the block and
  dodge and the damage are F8's, unchanged.
- It ends when someone is down, someone has pain 0.25 or blood loss 0.1, five game minutes have passed,
  either is drafted or called up by the alarm or taken under the player's hand, or one is gone.
  Each then thinks -20 of the other for 30 days ("got in a fight with me"), and neither starts a talk for
  20 minutes. Events `SocialFight` "started" and "ended: why".

## Social skill, cheating, jealousy, wedding parties

Extras after C20 (numbers are ours, in `SocialRules`).

- **Social skill.** A listener's opinion memory from a good chat (a positive offset) is multiplied by
  `1 + skillOpinionPerLevel (0.05) x the talker's Social level`; a slight or insult is not softened. The
  chance of a romance or proposal is multiplied by `1 + skillRomancePerLevel (0.02) x level` (still at most 95%).
- **Cheating.** A pawn with a lover or spouse it thinks less of than `cheatPartnerBelow` (60) may make romance
  attempts on a free pawn, at `cheatWeightFactor` (0.15) of the usual weight. The new bond forms and the old one
  stays (`stats.cheats`).
- **Jealousy.** Each betrayed partner holds *cheated on me* (-40, 60 days) about the cheater and *took my
  partner* (-20, 60 days) about the new lover, gets the *my partner cheated on me* mood thought, and with
  `jealousFightChance` (25%) fights the new lover with fists (never a Wimp or a Kind pawn). Event `Interaction`
  "Cheated" (`thing` the cheater, `target` the betrayed).
- **Wedding party.** When a proposal works, the couple get *my wedding party* (on top of *got married*) and every
  other colonist awake and free to talk gets *attended a wedding* (`stats.weddingGuests`; event `Interaction`
  "WeddingParty", `count` the guests). Every free colonist
  within 80 m on the same level also gets a `Party` job (`JobSystem::startParty`, `PartyJobs.cpp`): it drops its
  work, walks to its own cell round the couple, stands there for an hour (`partyMinutes`) gaining 0.4 recreation an
  hour (`partyJoyPerHour`), chatting with whoever is in earshot, and hunger or tiredness wait until it is over
  (`stats.partyGuests`). Ours; rules in `SocialRules`.

## Not yet

- Orientations and attractiveness, ideology-style rivalry, the "mutual friend" thoughts, and mood effects of
  seeing a friend die in front of you. Social skill for trade and recruiting (C22, C23). The experience rate
  follows the D9 numbers (10 to 30 an interaction), slow against `SkillRules.xpToNextLevel`; revisit with C12.
- The colonist panel's relations tab and the bond lines (U task; the data is `opinionLines` and
  `partner`, and the snapshot's `partner` and `socialFight`).
- Prisoners and recruiting (C22), mental-break insults (C19: an insulting spree can call `interact`).

## What Unreal sees

- **Snapshot:** per pawn `partner` (spouse, else lover) and `socialFight`.
- **Events:** `Interaction` (`thing` the talker, `target` the listener, `message` the interaction, `count`
  1/0, `amount` the listener's opinion after), `BondChanged` (`message` the kind, `count` 1 formed),
  `SocialFight` ("started", "ended: why").
- **Reads:** `sim.social().opinion / opinionLines / friends / rival / partner / bondBetween`.
- **Command:** `InteractCommand{initiator, recipient, interaction, outcome}` (dev; `outcome` 1/0 forces a
  roll that could go either way, including whether an insult becomes a fight, -1 leaves it to the dice).
  It ignores the opinion limits and the kind-words mood; it does not ignore what an interaction needs
  (a lover, adults) or whether the two can talk (alive, awake, in earshot).

## Save and hash

Section `social` (version 1): per pawn its `Social` (`nextTalk`, `mourned`, memories, bonds) and
`SocialFight`. Older saves have none and the components are made on the first step. Hashed in
`Sim::stateHash`. A new sim feature that acts on pawns must skip pawns with `SocialFight` as it skips
`Fighter`.
