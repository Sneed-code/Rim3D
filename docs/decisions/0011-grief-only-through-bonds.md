# 0011: Deaths cost mood only through bonds

Date: 2026-11-17. Asked by sneed.

**Decision.** Seeing a corpse, seeing someone die and "a colonist died" no longer cost any mood. A death is felt only
by those with a bond to the dead: a spouse, lover or friend (and a rival's death cheers), plus "saw someone I care
about die" for those bonded and close by.

**What changed.** The thoughts `KnowColonistDied`, `WitnessedDeathAlly`, `WitnessedDeathNonAlly` and
`ObservedLayingCorpse` are gone from `data/defs/Thoughts/Thoughts2.json`, and `MoodSystem` no longer gives
`KnowColonistDied` to every colonist on a colonist's death. The bond thoughts (`SpouseDied`, `LoverDied`, `FriendDied`,
`RivalDied`, `SawDeath`, Social.cpp) are unchanged. Butchering a person, wearing a dead man's clothes and executions
are not about seeing death and keep their thoughts.

**Why.** sneed wants fights and their aftermath to be about the people who mattered to each other, not a colony-wide
mood tax for every body on the ground.
