# Roads and bridges

Code: `Private/MapRoads.cpp` (`placeMapRoads`), the bridge deck in `Map` (`hasDeck`, `deckHeight`, `bridgeable`,
`setDeck`, `clearDeck`), `bridgeKindOf` / `buildingFitsCell` in `Things`, the blast rules in `CombatSystem::explode`,
`Sim::dropLostDecks`. Defs: `data/defs/Things/BuildingsBridges.json`, `PackedDirt` and `BrokenAsphalt` in
`data/defs/Terrain/Floors.json`. Tests: `Tests/RoadBridgeTests.cpp`. sneed asked 2026-11-19.

## Roads on the map

The world map already has roads between settlements (world-map.md step 8: dirt, stone, asphalt). A new map made for a
tile (`WorldSite::roadEdges`, the road on each of the tile's eight sides) gets **one road in from each side that has
one**, all meeting near the middle (40 to 60% across each way, from the seed). With no world (tests, plain `simrun`)
there are no roads; maps under 256 m and flat maps get none.

- **Route:** the cheapest way on a grid of 4 m blocks (8 ways): a flat dry block costs 10, plus 3 per 10 cm of rise
  across it, 20 per rock cell (4 or more closes it), 3 per deep-water cell, 2 per shallow, 1 per marsh or mud. A block
  already on a road costs 3, so later roads join the first (the best road is laid first). Then smoothed (Chaikin, 3
  passes) and painted at its width. Integer costs, so Linux and Windows give the same map.
- **Surface:** a road is a floor on the ground layer (`Floors`), so saves keep it: **packed dirt 3 m wide** (dirt
  road), **flagstone 4 m** (stone road), **broken asphalt 6 m** (asphalt road). The surfaces are RimWorld's (wiki:
  dirt road packed dirt, stone road flagstone, ancient asphalt broken asphalt); the widths are ours. Packed dirt and
  broken asphalt have no cost, so they are not on the Floors menu and can't be planned. Plants on the road go; a
  ruin's wall in the way is left standing.
- **Bridges:** where the road crosses deep or shallow water, an **old stone bridge** (`AncientBridge`, one building a
  cell) spans it at the road's width. Marsh and mud get the road surface. RimWorld lays wooden bridge floors here; the
  stone ones are ours (sneed: "places with roads have natural bridges over rivers").

Placed after the ruins and geysers when a new game's map is made (own random stream); not when loading.

## Bridges

A bridge is a **building, one per cell**, that lays a **deck**: the cell can be walked even over deep water, at the
deck's height, `Map::kDeckLiftM` (0.25 m, ours) above the water (or the cell's highest corner where that is higher).
`Map::heightAt` gives the deck's height there, so pawns are drawn on it, stand dry (`waterDepthAt` 0), walk at full
speed (no path cost, as RimWorld's 100% move speed), and shots and blasts meet it. The regions see the cell as
walkable, so pathing crosses it and goes round a broken span.

| Def | Goes on | Cost, work | Hit points | Explosions |
| --- | --- | --- | --- | --- |
| `Bridge` (wooden) | shallow water, marsh, mud | 12 wood, 1500 ticks | 100, flammability 80% | a Bomb blast of 35 or more destroys it at once |
| `HeavyBridge` | those and a river's deep water | 14 steel, 2500 ticks | 300 (ours) | immune (RimWorld 1.6), not flammable |
| `AncientBridge` | any water (map generation only) | not built | 300 (ours) | ordinary building damage |

RimWorld's numbers from the wiki (2026-11-19). Ours: a heavy bridge goes on a river's deep water (RimWorld: chest-deep
moving water) and needs no research yet (the Heavy bridges research is not in our tree).

**When a bridge goes** (blown up, burnt, shot, deconstructed): the deck goes with it. Items on a cell that is now deep
water are swept away (destroyed); in shallow water, marsh or mud they stay. A pawn left in deep water swims for the
nearest shore by itself (water.md). Unreal gets `ThingDespawned` and `ThingDestroyed` for each span, which is the cue
for the collapse effect (planks or stones falling, a splash).

**Rebuilding:** plan a new bridge on the gap; builders work from the bank or the deck beside it.

## Not yet

- Things built on a bridge (RimWorld lets light buildings and floors stand on one). Here the bridge is the cell's
  building, so nothing else goes there.
- Repairing damaged buildings in general (RimWorld's Repair work): no building repair job exists yet.
- Spans that fall when the cells holding them up go; caravan speed on roads.
- Unreal: the deck drawn at its height (it is drawn on the river bed until the BuildingSubsystem change), road
  surfaces in the terrain material, real bridge models (see "Look" below).

## Look (Unreal, planned)

Photoreal, modular and destructible: each cell's span is assembled from pieces (deck planks or slabs, railing posts
and rails on the outer edges, a pier every few metres in the water, abutments at the banks), chosen by looking at the
neighbouring cells. Models from free sources first (Fab's free Megascans: timber planks, stone blocks, rope; free
bridge kits), a Codex job only for what is missing. When a span is destroyed the pieces turn into Chaos physics
debris that tumbles into the water and fades, which is cosmetic only: the sim has already decided the span is gone.
Roads: the terrain material's gravel and dirt layers with a road tint first, then Megascans road decals.
