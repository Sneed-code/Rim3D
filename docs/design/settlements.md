# A settlement on its map (W6)

When a caravan first walks onto a faction settlement's tile (W5's `Visit`), that map gets the faction's base and its
people. Sim only: Unreal sends `TradeCommand` and `GiftCommand` with `map` = the tile and reads `Snapshot::otherMaps`.
Code in `Settlements.cpp` (`Sim::buildSettlement`) and `Trade.cpp` (`TradeSystem::startSettlement`), numbers in
`data/defs/Rules/SettlementRules.json` and `TradeRules`, tests `Tests/SettlementTests.cpp`, CTest `simrun_visit`.

## The base

Built once, when the map is made, and kept as anything else on the map is (walls can be shot down, beds slept in).
The layout has its own random stream from the world seed and the tile, so a settlement always looks the same.

- **Where.** A square of `baseSize` (34 to 38) cells, at the most open spot (fewest rock, water and building cells)
  at least 8 cells from the edge, nearest the middle among the best. Plants inside are cleared.
- **What of.** Walls and doors of a stuff the faction's tech level allows (`wallStuff`: tribes wood; industrial
  steel, wood or stone blocks; spacer steel or plasteel), furniture of `furnitureStuff`.
- **A wall round it** for `perimeterTechLevels` (industrial and spacer) on the square's edge, with a three-cell gate in
  the middle of each side. Tribes have none.
- **Rooms.** The square less two cells all round is cut into `blocks` x `blocks` (3 x 3). The middle block is the yard;
  each other block is a room by `roomChance` (85%): its walls are the block less a one-cell lane all round, its door in
  the middle of the side facing the yard (a corner room picks one of its two). The room's kind is picked by weight:
  barracks (beds side by side along two walls), dining room (a table and stools), kitchen (a stove), workshop (a
  crafting spot and a tailor's bench), storeroom (dressers). Things go in row by row from the room's low corner where
  they fit, never on the cell inside the door. Torch lamps and stoves start full of fuel.
- **The yard.** The trader's spot is the base's middle; a campfire stands beside it.
- Everything is built as construction finishes a building (`placeBlueprint`, every material delivered, `complete`),
  so it has its stuff, hit points and roofs as a colony's would. Nothing is loose on the ground: on another map the
  colony's goods are the items lying there, so the base brings none.

RimWorld's base generator cuts rooms at random, lays sandbags and turrets on an edge defense and stocks storerooms;
ours is the simpler grid above (all of it `ours` in the rules except the size, recalled from RimWorld).

## Its people (`VisitKind::Settlement`)

The people are a visit of their own kind in that map's trade system: they never leave, and the visit ends only when
none are left. `residents` (8 to 12, ours: RimWorld's 1150 to 1600 points of defenders over a gunner's ~130) pawns are
made in the yard, with their faction's looks, weapons and clothes as visitors are.

- **At peace** (the faction is not hostile to the colony): a trader of the faction's `traderKinds` with one of its
  `traders.base` kinds (if it trades), then half guards (`guardKinds`) and half villagers (`visitorKinds`). They carry
  `Visitor` (nobody's enemy) and walk in the Flee mode: the trader stands at the spot, the others go to a random cell
  within the base (`range`, half its size) every 15 minutes (`wanderEverySeconds`).
- **At war** (a permanent enemy such as the pirates, or goodwill at -75 or less): `pawnKinds` fighters without
  `Visitor`, in the Fight mode, each posted at a random cell in the base. Phase `Defending`: no trade, no gifts.
- **Turning hostile.** Every 15 seconds the map's trade system asks whether the faction is hostile now; if so the
  people stop being visitors and fight (`defend`). Becoming friends again doesn't make them visitors again (BACKLOG).
- **The old crews** have no pawn kinds: their settlements stand empty.

## Trading there

`TradeCommand` and `GiftCommand` take `map`: -1 for the colony's map, else the tile of the other map the visit is on
(refused with "no caravan has been to that tile" when there is none). The visit, the negotiator (a colonist on that
map) and the goods are that map's; the colony's goods and silver there are the items lying on that map (what the
caravan put down when it walked on). Prices are C23's with RimWorld's settlement offset: `settlementDiscount` (2%) is
added to the negotiator's price improvement, within the cap. The trader's stock and silver are rolled when the
settlement is made and again every `settlementRestockDays` (30, recalled from RimWorld), replacing what was left.

## Goodwill is the colony's

Another map's trade system has no standings of its own: `TradeSystem::setGoodwillOwner` points it at the colony's
(set when the map is made and when a save loads it, not saved). So a gift there, a resident killed there (-25) and
the faction's hostility are the colony's, and `GoodwillChanged` is a colony event.

## Snapshot and saves

`OtherMapView` adds the settlement's `faction`, its people's `visit` id on that map (for the commands), how many
`residents` are left and whether they are `hostile`. Section `trade` 2 adds the visit's `restockAt` and `range` and
allows the new kind and phase; `commands` 4 adds `map` to `Trade` and `Gift`. Replays write `map` only when it is not
-1, so older files read the same.

## Check it

`simrun --seed 42 --size 128 --days 3 --on-world --start Crashlanded --caravan --visit --check` (CTest
`simrun_visit`): at the settlement's map an hour after walking on, the colonists count its buildings, beds, doors and
people and sell its trader three survival meals; the check fails if there is no base, nobody lives there or a
friendly trader paid nothing. `--visit-png FILE` draws the map with the base.

## Not yet (in BACKLOG.md)

Attacking and taking a settlement (destroyed bases, loot, the faction's anger), its people using their beds,
tables and stoves and needing food, a hostile settlement making peace again, turrets and sandbags on the edge defense,
the trader's stock seen from the world map without walking on, Unreal drawing the map and the trade window there.
