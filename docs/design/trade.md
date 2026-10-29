# Trade, visitors and goodwill

Code: `ColonySim/Trade.h`, `Private/Trade.cpp` (`TradeSystem`). Tests: `Tests/TradeTests.cpp`. Task C23. Data: `Rules/TradeRules`,
`Rules/FactionRules`, `Factions`, `PawnKinds`, `TraderKinds` (all from D10, see [narrator-data.md](narrator-data.md)).

Other factions' people come to the colony in **visits**. A visit is one of three kinds:

| Kind | Who | Stays | Trade |
| --- | --- | --- | --- |
| `Visitors` | 2 to 5 villagers of the faction (`visitorKinds`) | 6 to 14 hours (ours) | none |
| `Caravan` | one trader (`traderKinds`) and 2 to 4 guards (`traderGuardKinds`) on foot | 12 to 20 hours (ours) | with the trader pawn |
| `Orbital` | a trade ship: no pawns, only a stock | 16 hours (`orbitalStayHours`, RimWorld) | while it is here |
| `Settlement` | a settlement's people on its own map (W6, [settlements.md](settlements.md)) | for good | with its trader, 2% better |

`TradeSystem::startVisit(faction, kind, traderKind, edge, id, error)` starts one. **T1's incidents call it** (the incident defs
`TraderCaravanArrival`, `VisitorGroup` and `OrbitalTraderArrival` already name the way); the narrator's workers for them (in `Sim.cpp`) pick a faction at random that can and is not hostile and call it, so the storyteller now sends caravans, visitors and orbital traders by itself.
`StartVisitCommand{faction, kind, traderKind, edge}` is the same for tests and dev. A faction must have `canVisit` (visitors) or
`canTrade` (the others) and must not be hostile (`permanentEnemy`, or goodwill at -75 or less). The trader kind is the one asked for or
one of the faction's `traders.caravan` / `traders.orbital` kinds by chance.

## Goodwill

Each faction's goodwill with the colony runs from -100 to 100 (`FactionRules`). A faction starts at a number inside its
`startingGoodwill` range, worked out from the world seed and the faction's name (no random draw, so the stream of everything else is
untouched). Only factions whose goodwill has changed are stored (`TradeState::standings`); they drift back toward their start by
`driftTowardNaturalPerDay` (0.2, ours) a day, and a permanent enemy stays at -100. `adjustGoodwill(faction, delta)` and
`AdjustGoodwillCommand` change it; at -75 or less a faction is `Hostile`, at 75 or more `Ally`. Goodwill changes by these rules
(`FactionRules.goodwillChanges`; the first two are recalled from RimWorld, the others ours):

| Cause | Change |
| --- | --- |
| `GiftCommand{visit, items}`: items lying on the map go to the visit's faction (silver counts 1 each) | market value / 40 (`giftSilverPerPoint`) |
| A prisoner of the faction is released (`PrisonerReleased`) | +12 (`releasePrisoner`) |
| A pawn with `Visitor` dies | -25 (`visitorKilled`); any death counts, whoever caused it, as events carry no attacker |
| `TradeSystem::noteExecuted(prisoner)`, for the executions task | -10 (`prisonerExecuted`) |

Trading itself changes nothing. A faction that falls to -75 or less (`hostile`) sends its staying visits away at once, sends no new
ones, and the narrator's raids may come from it (`NarratorSystem::setHostileCheck`) as from the permanent enemies. It is friendly again
once drift (0.2 a day) or gifts bring it above -75.

## Visitors and traders on the map

The pawns walk in from a map edge and gather at a spot near the colony's middle (the average position of its colonists), made like raiders
(`generateColonist`, looks, the pawn kind's weapons and clothes) with the `Visitor{visit, role}` component and the faction's `Faction`
id (`RaidSystem::factionId`). **They are nobody's enemy**: fighters (`PawnInfo::animal`), turrets, the alarm, wardens and the capture
rule skip a pawn with `Visitor`, as they do for prisoners. Nor are they anyone's: a visitor or prisoner never picks a victim within arm's reach either (`FighterSystem::meleeAct`; C25
found caravan guards knifing the colonists they stood beside). They walk as fighters in the Flee mode (so they never shoot) with a goal the
trade system sets: the spot while they stay, a random cell within 9 m of it every 15 minutes for the `Visitor` role (the trader and guards
stand), the nearest map edge when the time is up. Pawns at the edge are removed (`RaidSystem::leaveMap`, with their gear); any still on
the map 6 hours after the order to leave are removed anyway. A visit ends when its pawns are all gone or dead, or at the ship's time.
Not yet: visitors using chairs and tables, eating, becoming hostile when attacked, pack animals, a caravan fighting beside the colony
(BACKLOG).

## Prices

The colony **buys** at `colonyBuysAt` (140%) of a thing's `stats.marketValue` times `1 - improvement`, and **sells** at `colonySellsAt`
(60%) times a category factor (`sellCategoryFactors`: weapons 0.2, furniture 0.7, techprints 0.1, sculptures 1.1) times `1 + improvement`.
The improvement is the **negotiator's**: the Social skill along `priceImprovementBySocial` (0 at level 0, 30% at level 20, the wiki's
1.5% a level), plus the protagonist's talent (`1 - TalentSystem::statFactor(pawn, "tradePrice")`, P3's Haggler and Silver tongue; ours),
minus the difficulty's trade price disadvantage (`setPriceDisadvantage`, wired by T1), between -50% (ours) and +39.5% (the wiki's cap). A thing
with no market value can't be traded; silver is the money and is never a line.

What a trader takes from the colony is its kind's `buys` words matched against the thing's words (`thingCategories`, plus words made
from the def: `Apparel`, `Weapons`, `Metals`, `Stuff`, `Textiles`, `Foods`, `Bionics`), or any of `alwaysAccepted` (silver aside). A kind with
`refusesRawPlants` skips raw plant food. A trader's stock is the kind's `stock` list rolled at arrival (things the game doesn't have yet
are skipped) and its silver is rolled in the kind's `silver` range. What the colony sells joins the stock.

## The trade command

`TradeCommand{visit, negotiator, buy, sell}` (`TradeSystem::trade`): the lists are `{thing defName, count}`. It is **all or nothing**.
The negotiator is any pawn of the player's that can act (the trade window picks one; Social decides the price). The colony's goods and
silver are the **items lying on the map** (RimWorld uses items beside a trade beacon; ours has no beacon yet, so every item counts, taken
lowest id first; items worn or carried don't). The deal is paid in whole silver: the colony pays the buy side rounded up and gets the sell
side rounded down. If the colony owes silver it must have it (`silverOnMap`); if the trader owes, it pays what silver it has and **takes the
rest of the goods free** (RimWorld). Bought goods and the silver paid are put down in stacks near the visit's spot (the trader's
gathering place, or for a ship the colony's middle). A ship can be traded with from anywhere; a caravan's trader must be alive and up.
Event `TradeDone`.

## Interface

- Commands: `StartVisitCommand`, `TradeCommand`, `GiftCommand`, `AdjustGoodwillCommand` (saved and replayed by name).
- Events: `VisitArrived`, `VisitLeft`, `TradeDone`, `GoodwillChanged` (the visit id is in `Event::squad`).
- Snapshot: `visits` (id, kind, phase, faction, trader pawn, leave time, silver, members, spot, stock) and `goodwill` (every faction,
  with its relation); each `ThingView` has `visitor`, `visitRole`, `visit`.
- Quotes for the trade window (U19): `TradeSystem::unitPrice`, `priceImprovement`, `accepts`, `colonyHas`, `silverOnMap`.
- Save: section `trade` 1 (visits with stock, members and trader; standings; the visit counter; the last goodwill drift day) and the
  `Visitor` component. The state hash adds nothing until a visit or a goodwill change has happened, so old replays keep their hashes.
