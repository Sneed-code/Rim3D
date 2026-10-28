# Comms console and orbital trade beacon

Two buildings that give the `Microelectronics` project more to unlock (BACKLOG "Research tree unlocks without content").
Numbers from the RimWorld wiki, checked 2026-10-28. Code: `TradeSystem` (`Trade.h`, `Private/Trade.cpp`). Tests: `TradeTests.cpp`
("Comms: ...").

| Def | Numbers |
|---|---|
| `CommsConsole` | 120 steel + 4 components, 2200 ticks (3168 s), 250 HP, 3 x 2, 200 W, 60% flammable. `building.commsConsole`. |
| `OrbitalTradeBeacon` | 40 steel + 1 component, 800 ticks (1152 s), 75 HP, 1 x 1, 40 W, 50% flammable. `building.tradeBeacon`, `building.tradeRadius` 7.9 (recalled from RimWorld's code). |

## Rules

- **Trade ships need a console.** The narrator's `OrbitalTraderArrival` fails (and the narrator draws another incident) unless the
  colony has a built, powered comms console. `StartVisitCommand` (dev) still starts one anyway.
- **Trading with a ship** (`TradeCommand`, `GiftCommand`) needs a powered console the negotiator can walk up to (any walkable cell
  beside it that the negotiator can reach) and at least one powered trade beacon. The negotiator does not walk there yet; the
  check is only that it could (BACKLOG).
- **Beacon cells.** What the colony sells, gives or pays to a ship must lie on a beacon's cells: cells within 7.9 m of a powered
  beacon that are reached from it without crossing a wall, a door or rock (RimWorld: the beacon's district). `beaconCells()`,
  `beaconHas(thing)`. What the colony buys, and silver it is paid, goes down beside the first beacon (lowest id).
- **Asking an ally for a caravan.** `CommsCommand{"RequestCaravan", faction, traderKind, negotiator}`: the faction must trade and
  be an ally (goodwill 75 or more); the caller must be a colony pawn who can act and reach a powered console. It costs 19
  goodwill, the same faction can't be asked again for 4 days, and the caravan walks in 2 days later (`TradeRules.comms`; the wait
  is recalled from RimWorld's code, unverified). `traderKind` may name one of the faction's `traders.caravan` kinds. Events
  `CaravanRequested` (`amount` days to wait) and, if the caravan can't come when due (the faction turned hostile), `CaravanRequestFailed`.
- Caravans on foot and settlements trade as before: every item on the map counts.

## Save and hash

Section `comms` 1: the waiting requests and when each faction was last asked (`TradeState::requests`, `calls`); a call is forgotten
when its cooldown ends. Both join `TradeSystem::hashState` only once a caravan has been asked for, so older replays keep their hashes.

## Not yet

Military aid (31 goodwill, 24 h cooldown), other comms options, the negotiator walking to the console, a solar flare cutting it,
unsafe weather blocking requests, beacon areas drawn in Unreal, and the trade window using `beaconHas` for ships (U19 follow-up).
