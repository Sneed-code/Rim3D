#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/World.h"

#include <cstdint>

namespace colony {

class Floors;
class LootSystem;
class Map;
class Mineables;
class Plants;
class Things;

struct MapPropCounts {
    int roadCells = 0;     // ground cells given a road floor
    int dangerRooms = 0;   // sealed ancient danger rooms
    int dangerWalls = 0;   // AncientWall buildings round them
    int caskets = 0;       // AncientCryptosleepCasket buildings in them
    int lootStacks = 0;    // stacks of the AncientDanger loot table put in them
    int wrecks = 0;        // ShipChunk buildings
    int slag = 0;          // ChunkSlagSteel items beside the wrecks
    int boulders = 0;      // Boulder<Rock> buildings
    int fallenLogs = 0;    // FallenLog buildings
    int chunks = 0;        // stone chunk items
};

// What placeMapProps needs. Any pointer may be null: what needs it is left out.
struct MapPropInput {
    Map* map = nullptr;
    Things* things = nullptr;
    Plants* plants = nullptr;              // cleared under what is placed; fallen logs lie near trees
    Floors* floors = nullptr;              // roads are floors
    const Mineables* mineables = nullptr;  // which rock the boulders and chunks are
    LootSystem* loot = nullptr;            // fills the danger rooms
    RoadKind road = RoadKind::None;        // the world tile's road, if any
    uint64_t seed = 0;
};

// The rest of what a new map is made with, after placeMapFeatures' ruins and geysers
// (docs/design/mapgen.md "Ground props", "Wreckage", "Ancient danger", "Roads"): a road across the
// map when the world tile has one, sealed ancient danger rooms with caskets and loot, ship wreckage
// with steel slag, boulders, fallen logs and clusters of stone chunks. Numbers from the
// MapPropRules def. Own random stream from `seed`. Maps under 256 m get none.
COLONY_API MapPropCounts placeMapProps(const MapPropInput& in);

}  // namespace colony
