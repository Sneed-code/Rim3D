#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"

#include <cstdint>
#include <string>
#include <vector>

namespace colony {

// The world map (docs/design/world-map.md): a planet of tiles as a wrapping grid (east wraps to
// west, the top and bottom rows are the poles), made from a seed and the WorldGen def. It is a
// pure function of (seed, size, defs), so it is regenerated, not stored; a save keeps only the
// seed, size and the start tile.

enum class Hilliness : uint8_t { Flat, SmallHills, LargeHills, Mountainous, Impassable };
enum class RiverSize : uint8_t { None, Creek, River, LargeRiver, HugeRiver };
enum class RoadKind : uint8_t { None, Dirt, Stone, Asphalt };

COLONY_API const char* hillinessName(Hilliness h);
COLONY_API const char* riverSizeName(RiverSize r);
COLONY_API const char* roadKindName(RoadKind r);

constexpr uint8_t kNoBiome = 255;
constexpr int kNoSettlement = -1;
constexpr uint8_t kNoDirection = 255;

struct WorldTile {
    int16_t elevation = 0;         // metres above sea level; negative under water
    float temperature = 0.0f;      // yearly mean, degrees C
    float rainfall = 0.0f;         // mm per year
    uint8_t biome = kNoBiome;      // index into World::biomeName; kNoBiome for open water
    Hilliness hilliness = Hilliness::Flat;
    RiverSize river = RiverSize::None;
    uint8_t riverFlowsTo = kNoDirection;  // 0..7 into World::kNeighbourDx/Dy; kNoDirection if no river
    RoadKind road = RoadKind::None;       // best road on any edge of this tile
    bool water = false;            // ocean or lake (sea ice is water with a biome)
    int32_t settlement = kNoSettlement;  // index into World::settlements()
};

struct WorldSettlement {
    int tile = 0;
    std::string faction;  // Factions defName
    std::string name;
};

struct WorldRoad {  // one road edge between two neighbouring tiles
    int tileA = 0;
    int tileB = 0;
    RoadKind kind = RoadKind::Dirt;
};

// What a map made for a tile needs to know about where it is (the S8 map generator reads this).
struct WorldSite {
    int tile = 0;
    std::string biome;
    Hilliness hilliness = Hilliness::Flat;
    RiverSize river = RiverSize::None;
    RoadKind road = RoadKind::None;
    float temperature = 0.0f;
    float rainfall = 0.0f;
    int elevation = 0;
    uint64_t mapSeed = 0;  // seeds the tile's 1 km map: same world and tile give the same map
};

// What a start tile must be like. Empty fields mean "don't care".
struct StartCriteria {
    std::string biome;
    float minTemperature = -1000.0f;
    float maxTemperature = 1000.0f;
    bool needRiver = false;
    bool needRoad = false;
    uint64_t seed = 1;  // which of the matching tiles comes first
};

class COLONY_API World {
public:
    static constexpr int kNeighbourDx[8] = {0, 1, 1, 1, 0, -1, -1, -1};  // N, NE, E, SE, S, SW, W, NW
    static constexpr int kNeighbourDy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};

    // Empty world (no tiles) when the defs have no WorldGen "Default". width and height 0 take the def's.
    static World generate(const DefDatabase& defs, uint64_t seed, int width = 0, int height = 0);

    uint64_t seed() const { return seed_; }
    int width() const { return width_; }
    int height() const { return height_; }
    int tileCount() const { return width_ * height_; }
    bool empty() const { return tiles_.empty(); }

    int index(int x, int y) const { return y * width_ + x; }
    int tileX(int index) const { return index % width_; }
    int tileY(int index) const { return index / width_; }
    const WorldTile& tile(int index) const { return tiles_[static_cast<size_t>(index)]; }
    // The neighbour in direction dir (0..7), or -1 past a pole. East and west wrap.
    int neighbour(int index, int dir) const;

    const std::string& biomeName(const WorldTile& t) const;  // "" for open water
    const std::vector<std::string>& biomeNames() const { return biomes_; }
    const std::vector<WorldSettlement>& settlements() const { return settlements_; }
    const std::vector<WorldRoad>& roads() const { return roads_; }
    int landTileCount() const;

    // The start tile: -1 until chosen. A tile may start a colony when it is land, not impassable
    // mountain, not open sea ice and has no settlement.
    int startTile() const { return startTile_; }
    bool canStartAt(int index) const;
    bool chooseStartTile(int index);
    // Another map's tile (W5, docs/design/other-maps.md): any land a caravan can stand on, settlements too.
    bool canVisit(int index) const;
    // Makes `index` the tile this world's sim is the map of, for a visit; false (and nothing changes) if it can't be visited.
    bool chooseVisitTile(int index);
    int suggestStartTile(const StartCriteria& criteria) const;  // -1 if nothing matches

    WorldSite siteFor(int index) const;
    // A hash of every tile, settlement and road, for tests and replays.
    uint64_t hash() const;

private:
    uint64_t seed_ = 0;
    int width_ = 0;
    int height_ = 0;
    std::vector<WorldTile> tiles_;
    std::vector<std::string> biomes_;
    std::vector<WorldSettlement> settlements_;
    std::vector<WorldRoad> roads_;
    int startTile_ = -1;
    int maxStartHilliness_ = 2;

    friend struct WorldBuilder;
};

}  // namespace colony
