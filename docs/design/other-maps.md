# Other maps (W5)

A caravan on another world tile can walk onto that tile's map. The map is made the first time a
caravan visits the tile, from the tile's site, and kept from then on: things left there are still
there on the next visit, and it keeps running while the colony runs. Sim only: Unreal sends
`CaravanCommand` and reads `Snapshot::otherMaps`; drawing another map and giving orders there are
Unreal tasks for later. Code in `OtherMaps.cpp` (with `moveOffMapThings` in `Save.cpp`), numbers in
`data/defs/Rules/OtherMapRules.json`, tests `Tests/OtherMapTests.cpp`, CTest `simrun_visit`.

## Each map is a sim of its own

Another map is a whole `Sim` owned by the colony's (`Sim::otherMaps()`, `Sim::otherMap(tile)`). It
was the smallest way to get many maps from a game built around one: every system works unchanged on
it, and each map has its own biome, weather, plants, animals and things.

- **Made from the tile's site.** Its settings: the colony's world with `startTile` = the tile and
  `world.visit` on (any land a caravan stands on, settlements too), so `Sim::site()` gives its biome,
  climate and `mapSeed` (the same tile always gives the same ground); its own seed from the colony's
  and the tile; the colony's wild animal density and path budget; no scenario and no narrator.
- **Size** (ours, `OtherMapRules`): 256 cells a side, 384 with a settlement. RimWorld makes every map
  the colony's size; our game design gives other maps 256 to 512 m so they are quick to make and cheap
  to keep running. An empty 384 map costs a few microseconds a step.
- **In step.** It starts at the colony's game second (`SimSettings::startSecond`, not saved) and is
  stepped once at the end of every colony step, night skips included, so its clock, season and
  weather time match. Its events stay its own (Unreal reads them when it draws that map).
- **Kept.** Nothing abandons a map yet: each visited tile's sim lives until the game ends.
- **A settlement's tile** gets the faction's base and its people when the map is made, and the map's goodwill is the
  colony's (W6, [settlements.md](settlements.md)); `TradeCommand` and `GiftCommand` with `map` = the tile trade there.

Transport pods can land on a map that exists already (not a settlement's), and pods sent to a tile with
`Caravan` arrival make a caravan there that can visit it ([transport-pods.md](transport-pods.md)).

## Moving pawns between sims

Things of one sim can't be named in another (entity ids are per sim). `moveOffMapThings` moves
things that are off their map, a caravan's pawns and the stacks it carries, with every part they
have: it reads them as a save does (`collectEntity`), takes along every off-map item they name (worn
apparel, the weapon, stowed guns, what they carry), writes and reads them through the save format
with every entity id turned into the other sim's, and puts the parts on new or given entities there
(`restoreEntity`), drug states too. A name of something that doesn't move (a bed, a job target, a
colonist who stayed) becomes "nobody"; memories and bonds to nobody fall away. Jobs, paths and
controllers are not carried over.

**The colony keeps a place for each colonist who is away.** A pawn that leaves the colony's sim
stays there as an entity with one part, `AwayPawn` (the tile, its entity on that map and its own
bonds and memories as it left). So beds, allowed areas and the bonds and memories of colonists who
stayed still name it. When it comes back the same entity is filled again: it is the same colonist
with the same id, with what happened on the trip (wounds, needs, skills, gear, ties among the
travellers) and its ties to those at home put back. A colonist who dies away, or is gone from that
map, loses its place within the hour. Pawns born or recruited there come back as new entities.

## Commands (`CaravanCommand`)

| action | fields | does |
|---|---|---|
| `Visit` | `caravan` | a caravan waiting or resting on a tile that isn't the colony's walks onto that tile's map (made the first time), at the edge facing the tile it came from, items on the ground beside it |
| `Form` with `map` = a tile | `pawns`, `items` (that map's things), `tile` | colonists on that map form a caravan there, as on the colony's map (W4's rules); the moment it sets out it becomes the colony's caravan, travelling to `tile` (or waiting on the visited tile for -1) |

Refused: no world, no such caravan, still forming, travelling (stop it first), at the colony (use
Enter), nobody in it, no map of that tile (for `Form`).

Events (on the colony's sim): `OtherMapMade` (`count` cells a side), `OtherMapEntered` (`count` the
caravan that walked on), `OtherMapLeft` (`count` the colony's new caravan); `amount` is the tile and
`message` the place (the settlement's name, else the biome's label). The visit itself runs W4's
`CaravanEntered`, `CaravanFormed` and `CaravanDeparted` on the other map's sim.

## Snapshot (`OtherMapView`)

Per map: tile, place name, cells a side, the colony's colonists there, caravans forming there; for a settlement its
faction, its people's visit id, how many are left and whether they are hostile (W6).

## Saves

Section `maps` 1: each map's tile, when it was made and its whole sim as a save of its own (loaded
with `loadSim`), then the `away` part of each placeholder (tile, entity there, its bonds and
memories). Section `world` 2 adds `visit`; `commands` 3 adds `map` to `Caravan`. The colony's state
hash takes in each map's hash and the placeholders, so checkpoints and replays cover other maps.

## Check it

`simrun --seed 42 --size 128 --days 3 --on-world --start Crashlanded --caravan --visit --check`
(CTest `simrun_visit`): two colonists take the meals to the nearest settlement, walk onto its map,
stay half a day, form a caravan there and come home; the map is kept. `--visit-png FILE` draws it.

## Not yet (in BACKLOG.md)

Quest sites, ambushes and caravan stops as
map kinds, abandoning a map, the narrator and raids on other maps, a death away telling the colony
(mood), orders on another map other than forming a caravan (needs a command that names the map),
Unreal drawing another map and switching to it, a night skip that counts colonists away.
