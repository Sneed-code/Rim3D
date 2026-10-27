# Narrator, incident and faction data (D10)

Data only: no sim code reads most of this yet. T1 (narrator and incident system) and C23 (visitors and
traders) are the readers; F6's raids already read `Factions` and `PawnKinds`. Test:
`Tests/NarratorDataTests.cpp`. Numbers come from the RimWorld wiki's Difficulty, Trade and Traders pages
where they could be read; everything else is recalled and listed in each def's `unverified`. Names are
ours ([naming.md](naming.md)).

## Narrators (`Narrators/`)

| Narrator | Style | Big threat | First threat |
| --- | --- | --- | --- |
| Orrin | steady build-up | one or two every ~10.6 days (4.6 on, 6.0 off) | day 11 |
| Maren | long quiet spells | one every 16 days (8 on, 8 off) | day 13 |
| Jinx | anything goes | each incident rolls alone (`mtbDays`), points x 0.5 to 1.5 | none |

`mtbDays` is the mean days between incidents per category. Maren scales points by 0.85.

## Difficulties (`Difficulties/`)

Peaceful, Community builder, Adventure story, Strive to survive (default), Blood and dust, Losing is
fun. Each has `threatScale` (0.1, 0.3, 0.6, 1, 1.55, 2.2), adaptation growth and impact, trade price
disadvantage, yields, research speed, mood offset and the disease, infection, food poisoning and
infestation rates: all from the wiki's Difficulty page. `RaidRules.threatScale` stays 1, the default.

## Incidents (`Incidents/`)

30 defs with `category` (ThreatBig, ThreatSmall, Misc, FactionArrival, OrbitalVisitor, Disease), `letter`
kind, `baseChance`, `earliestDay`, `cooldownDays` and, for threats, `points.factor`: raid, manhunter pack,
chitinid infestation, remnant cluster, mad animal, six diseases, blight, short circuit, solar flare,
toxic fallout, cold snap, heat wave, eclipse, aurora, wanderer, cargo pod, crashed pod, herd migration,
animal joins, berry and healroot bloom, trade caravan, visitors, orbital trader, prison break. The
chances and days are recalled, so tune them in T1 with `tuning.json`.

## Threat points

Already in `RaidRules` (F6): wealth and colonist curves, days-passed factor, adaptation. T1 multiplies
by the difficulty's `threatScale` and the narrator's `pointsFactor`. `NarratorRules` adds the
population-intent curve and the defaults.

## Factions (`Factions/`)

Pirate (permanent enemy), Fray Compact, free hold, gentle clan, fierce clan, savage clan (permanent
enemy), the Concord, old crews, the Remnant, chitinids. Fields: `startingGoodwill` range,
`permanentEnemy`, `canRaid/canVisit/canTrade`, `pawnKinds` (who raids), `visitorKinds`, `guardKinds`,
`traderKinds`, `traderGuardKinds`, `traders {base, caravan, orbital}`. The Remnant and chitinids have no
kinds until bodies for them exist. `FactionRules`: goodwill -100 to 100, hostile at -75 or less, ally at
75 or more.

New pawn kinds carry a `role` (raider, guard, visitor, trader, traderGuard): Fray villager, guard,
trader and trader guard; the same four for the free hold; shared clan villager, warrior, shaman
trader and trader guard; Concord envoy and trooper.

## Traders (`TraderKinds/`, `Rules/TradeRules`)

Kinds: bulk goods, combat supplier, exotic goods, clan shaman, slaver, war merchant, each with a silver
range, a stock list of things we have, and what it buys and sells (category words C23 will match).
`TradeRules`: the colony sells at 60% and buys at 140% of market value, settlements discount 2%,
category factors on selling (furniture 0.7, weapons 0.2...), Social skill improves prices (our straight
line until the formula is checked), orbital traders every 15 days for 16 hours, caravans five dates a
year.
