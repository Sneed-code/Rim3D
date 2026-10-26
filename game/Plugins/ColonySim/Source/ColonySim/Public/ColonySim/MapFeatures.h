#pragma once

#include "ColonySim/Api.h"

#include <cstdint>

namespace colony {

class Map;
class Things;

struct MapFeatureCounts {
    int ruins = 0;      // old ruined rooms placed
    int ruinWalls = 0;  // AncientWall buildings in them
    int geysers = 0;    // SteamGeyser buildings
};

// Puts the things a new map is made with onto it: old ruins (the outlines of rooms with broken walls
// and gaps to walk through) and steam geysers (docs/design/mapgen.md). Uses its own random stream
// from `seed`, so the rest of the sim's randomness is untouched. Needs the AncientWall and
// SteamGeyser defs; without them nothing is placed. Maps under 256 m get none.
COLONY_API MapFeatureCounts placeMapFeatures(Map& map, Things& things, uint64_t seed);

}  // namespace colony
