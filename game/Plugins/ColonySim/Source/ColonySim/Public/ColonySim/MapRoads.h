#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/World.h"

#include <array>
#include <cstdint>

namespace colony {

class Floors;
class Map;
class Plants;
class Things;

struct MapRoadCounts {
    int roads = 0;        // routes laid (one per road edge of the tile)
    int roadCells = 0;    // cells given a road surface (a floor)
    int bridgeCells = 0;  // AncientBridge buildings over water
};

// Lays the roads of a new map (docs/design/roads-bridges.md): one in from each side of the tile that has a road on
// the world map, all meeting near the middle, routed round rock and steep ground and across water by the shortest
// crossing. A dirt road is packed dirt 3 m wide, a stone road flagstone 4 m, an asphalt road broken asphalt 6 m.
// Where a road crosses water (deep or shallow) an old stone bridge (AncientBridge) spans it at the road's width.
// Plants on the road go. Cells with a building (a ruin's wall) are left alone. Uses its own random stream from
// `seed`. Needs the floor and bridge defs; maps under 256 m get none.
COLONY_API MapRoadCounts placeMapRoads(Map& map, Things& things, Plants& plants, Floors& floors,
                                       const std::array<RoadKind, 8>& roadEdges, uint64_t seed);

}  // namespace colony
