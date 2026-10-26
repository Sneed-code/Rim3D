# Quests (Q1)

Sim only. A quest is an offer from outside the colony: what is asked, what comes of it, and a few
days to answer. RimWorld's quests are far more varied (they need the world map, other maps and
caravans, which are Stage 3), so Q1 has four kinds that need no travel. All numbers are ours.
Code: `Quests.h` / `Quests.cpp`; defs `data/defs/Quests/Quests.json`, `Rules/QuestRules.json`;
tests `Tests/QuestTests.cpp`. Own names: the Pirate faction, silver and steel as in the other docs.

## Life of a quest

`Offered` (waits `offerDays`, then `Expired`) then the player answers: `Declined`, or `Active` once
accepted, and an active quest ends `Completed` (paid) or `Failed`. The list keeps the newest 20
finished quests. Limits (`QuestRules`): 4 open offers, 3 active quests, and never two open quests of
one kind. A kind can have an `earliestDay` and `minColonists`.

## The four kinds (`questType`)

| Type | Quest def | On accept | Ends |
| --- | --- | --- | --- |
| `Refugees` | `RefugeesChased` | 1 to 3 people walk in from a map edge as colonists; a raid (`faction`, `points`) is sent 1 to 3 hours later | `Completed` and paid when the raid is over; `Failed` if every refugee dies or the raid is still on after 24 hours |
| `Wanderer` | `WandererAsks` | one person walks in as a colonist | `Completed` at once |
| `Stash` | `SupplyCache` | the rewards land near the colony's middle | `Completed` at once |
| `Defend` | `HoldTheLine` | a raid is sent 6 to 12 hours later | `Completed` and paid when the raid is over; `Failed` after 24 hours with it still on |

A raid that can't be sent (no way in) ends a Refugees quest `Completed` ("no raid came") and a
Defend quest `Expired`. Rewards are rolled when the quest is offered (so the offer shows what it
pays) and spawned as stacks round the colony's middle; if there is no room the accept fails and
the offer stays open.

## Hooks for the narrator

- `sim.quests().offerRandom(id, error)` makes an offer of a kind chosen by `weight` among the kinds
  available now; `offer("HoldTheLine", id, error)` makes a named one; `available()` lists the kinds.
  Both fail with a reason in plain words when none can be offered.
- The same through `QuestCommand` (`action` "offer", `kind` empty for random), so a replay
  records what the narrator did. `accept` and `decline` take `quest` (the id).
- No `Incidents` def yet: T1 decides how often to call it (a `Misc` incident that calls
  `offerRandom` is enough).

## What Unreal sees

Snapshot `quests` (`QuestView`: id, kind, label, state, expiresAt, deadline, people, points,
rewards). Events `QuestOffered`, `QuestAccepted`, `QuestDeclined`, `QuestExpired`, `QuestCompleted`,
`QuestFailed` (`count` the quest id, `message` the def or the reason). New colonists come as the
usual `ThingSpawned`. Save section `quests` 1; the command is saved by name (`Quest`).
