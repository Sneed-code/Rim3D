#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace colony {

class Cover;
class Regions;
class Rooms;

// Placeholder terrain types until terrain defs are loaded from data/defs. Rock is solid until
// it is mined (docs/design/mining.md), and leaves RoughStone: walkable, nothing grows on it.
// The last two are only ever on upper levels (docs/design/levels.md): Air where nothing has been
// built, UpperFloor where an upper floor is laid and can be walked and built on.
// `Water` is deep water (nobody crosses it). Shallow water, marsh and mud can be walked, slowly; the
// `Terrain` defs of the same names carry their path costs (docs/design/mapgen.md).
// New values go at the end.
enum class Terrain : uint8_t { Water, Sand, Soil, Rock, RoughStone, Air, UpperFloor, ShallowWater, Marsh, Mud };
constexpr int kTerrainCount = 10;

// How the building in a cell affects movement. A door can be walked through but is a region of
// its own (docs/design/regions.md).
enum class Passage : uint8_t { Open, Blocked, Door };

// The roof over a cell. Rock is the mountain's own roof (thick) and stays when a cell is mined
// out; Thin is the roof that Rooms puts over enclosed rooms (docs/design/rooms.md), and what an
// upper floor is to the cell under it (docs/design/levels.md).
enum class Roof : uint8_t { None, Thin, Thick };

// A way between two levels: stairs or a ladder (task M2, docs/design/levels.md). A pawn steps on
// from `low`, the cell in front of its foot on the building's own level, and steps off at `high`,
// the cell beyond its head one level up; the two are side by side seen from above. Things keeps
// these in step with the buildings, and regions link the regions of the two cells.
struct LevelLink {
    CellPos low;
    CellPos high;
    Entity building = kNoEntity;
    float length = 0.0f;       // metres from the middle of `low` to the middle of `high`, climbing
    float speedFactor = 1.0f;  // share of its walking speed a pawn keeps on it

    // What the link costs a path, in metres of flat walking.
    float cost() const { return length / speedFactor; }
};

// How far apart two places are when choosing the nearest target, in square metres: the flat
// distance, and each level between them counts as kLevelDetourM more (ours: the walk round to the
// stairs and up them). Work scanners that look across levels use it (docs/design/levels.md).
inline constexpr float kLevelDetourM = 10.0f;
inline float levelDistSq(float dx, float dy, int levels) {
    const float up = kLevelDetourM * static_cast<float>(levels < 0 ? -levels : levels);
    return dx * dx + dy * dy + up * up;
}

struct MapSettings {
    int width = 512;   // cells, 1 m each
    int height = 512;  // cells, 1 m each
    uint64_t seed = 0;
    bool flat = false;  // every cell soil at height 0, for tests
    // How much river and how much marsh the biome gets, as multiples of the usual (0: none).
    float river = 1.0f;
    float marsh = 1.0f;
};

// The map grid: one flat array per layer, generated from a seed.
//
// The ground (level 0) is stored whole. Levels 1 to kLevels - 1 stack kLevelHeightM above it and
// are stored sparsely, in 16 m chunks made when something is first built there, so empty sky costs
// nothing (docs/design/levels.md). A cell on an upper level is Air until an upper floor is laid on
// it; then it can be walked and built on like ground, and is the roof of the cell below.
//
// The map owns its regions and tells them when a cell's passage changes. It can't be copied or
// moved, because its regions point back at it.
class COLONY_API Map {
public:
    explicit Map(const MapSettings& settings);
    ~Map();
    Map(const Map&) = delete;
    Map& operator=(const Map&) = delete;

    // Levels the map can hold: the ground and the upper levels above it.
    static constexpr int kLevels = 4;
    // How far each level is above the one below, in metres (ours: RimWorld has no levels).
    static constexpr float kLevelHeightM = 3.0f;
    // Side of an upper-level storage chunk, in cells; the same as a region block.
    static constexpr int kUpperChunk = 16;

    int width() const { return width_; }
    int height() const { return height_; }
    int levels() const { return kLevels; }
    bool inBounds(CellPos c) const {
        return c.level >= 0 && c.level < kLevels && c.x >= 0 && c.y >= 0 && c.x < width_ && c.y < height_;
    }
    // In bounds and on the ground level. Systems that only know the ground so far (plants, zones,
    // filth, lights, cover...) use this instead of inBounds (docs/design/levels.md).
    bool onGround(CellPos c) const { return c.level == 0 && inBounds(c); }
    // Index of the cell within its level's layers. The cell must be in bounds.
    int index(CellPos c) const { return c.y * width_ + c.x; }
    CellPos cellAt(int index, int level = 0) const { return CellPos{index % width_, index / width_, level}; }

    // Placeholder until terrain defs: water and rock can't be walked or built on, and neither can
    // the air of an upper level with no floor laid.
    bool groundWalkable(CellPos c) const {
        const Terrain t = terrain(c);
        return t != Terrain::Water && t != Terrain::Rock && t != Terrain::Air;
    }
    // Pawns can stand here: in bounds, walkable ground and no building that blocks movement.
    bool walkable(CellPos c) const { return inBounds(c) && groundWalkable(c) && passage(c) != Passage::Blocked; }
    // A walkable cell with a door in it.
    bool isDoor(CellPos c) const { return walkable(c) && passage(c) == Passage::Door; }

    // Height in metres of what a pawn stands on in the cell: the ground on level 0, the ground
    // plus kLevelHeightM per level above it (an upper floor runs parallel to the ground under it).
    // The cell must be in bounds.
    float groundHeight(CellPos c) const {
        return groundHeight_[static_cast<size_t>(index(c))] + static_cast<float>(c.level) * kLevelHeightM;
    }
    Terrain terrain(CellPos c) const {
        if (c.level == 0) [[likely]] {
            return terrain_[static_cast<size_t>(index(c))];
        }
        const UpperChunk* chunk = upperChunk(c);
        return chunk != nullptr && chunk->floor[upperSlot(c)] != 0 ? Terrain::UpperFloor : Terrain::Air;
    }
    // Changes a cell's ground (mining turns Rock into RoughStone). Regions and cover are told when
    // that changes what can be walked or seen through. Does not touch the ground height. Ground
    // level only: upper floors are laid with setUpperFloor.
    void setTerrain(CellPos c, Terrain terrain);
    // Height in metres at any point of a level, interpolated between the heights of the four grid
    // corners around it. The corner (x, y) is the point x metres by y metres, which is how the
    // terrain mesh is drawn, so pawns stand on the surface the player sees. Clamped to the map.
    float heightAt(float x, float y, int level = 0) const;
    // Ground-column shorthands for code that only reads the ground surface (the terrain mesh).
    float groundHeight(int x, int y) const { return groundHeight(CellPos{x, y, 0}); }
    Terrain terrain(int x, int y) const { return terrain(CellPos{x, y, 0}); }

    // Upper floors (docs/design/levels.md). True if the cell is on an upper level and has a floor.
    bool upperFloor(CellPos c) const { return c.level > 0 && terrain(c) == Terrain::UpperFloor; }
    // True if the cell one level up has an upper floor: that floor is this cell's roof.
    bool floorAbove(CellPos c) const {
        return c.level + 1 < kLevels && upperFloor(CellPos{c.x, c.y, c.level + 1});
    }
    // Lays or takes up the floor of an upper-level cell, with no checks of support or of what
    // stands on it (UpperFloors does those). False for a cell off the map or on the ground.
    bool setUpperFloor(CellPos c, bool floor);
    // True if the cell's layers are stored: always on the ground, on an upper level only once
    // something was built in its 16 m chunk. Regions skip chunks that aren't.
    bool hasUpperChunk(CellPos c) const { return c.level == 0 || (inBounds(c) && upperChunk(c) != nullptr); }
    // Every upper floor, by level then row, and how many times one was laid or taken up.
    std::vector<CellPos> upperFloorCells() const;
    uint64_t upperFloorChanges() const { return upperFloorChanges_; }
    // Hash of the upper floors, for Sim::stateHash; cheap, it only visits chunks that exist.
    uint64_t upperFloorHash() const;

    // Stairs and ladders (docs/design/levels.md), sorted by `low` then `high` (row order, levels
    // first). Adding or removing one tells the regions at both ends.
    void addLevelLink(const LevelLink& link);
    void removeLevelLinks(Entity building);
    const std::vector<LevelLink>& levelLinks() const { return levelLinks_; }
    // The link with `a` at one end and `b` at the other, either way round; null if none.
    const LevelLink* levelLinkBetween(CellPos a, CellPos b) const;

    // Building layer: the building covering each cell, kNoEntity if none. Kept up to date by
    // Things when buildings spawn and despawn; nothing else should call the setters.
    Entity buildingAt(CellPos c) const {
        if (c.level == 0) [[likely]] {
            return building_[static_cast<size_t>(index(c))];
        }
        const UpperChunk* chunk = upperChunk(c);
        return chunk != nullptr ? chunk->building[upperSlot(c)] : kNoEntity;
    }
    // `obstacleDm` is how tall the building is for sight and cover (see Cover.h); 0 for a
    // building that is neither.
    void setBuilding(CellPos c, Entity building, Passage passage, uint8_t obstacleDm = 0);
    void clearBuilding(CellPos c);
    // Changes only the height for sight and cover of the building in a cell; a door does it when it
    // opens (it stops blocking sight) and closes (docs/design/rooms.md).
    void setObstacle(CellPos c, uint8_t obstacleDm);
    Passage passage(CellPos c) const {
        if (c.level == 0) [[likely]] {
            return passage_[static_cast<size_t>(index(c))];
        }
        const UpperChunk* chunk = upperChunk(c);
        return chunk != nullptr ? chunk->passage[upperSlot(c)] : Passage::Open;
    }
    // How tall what stands in the cell is for sight and cover, in decimetres above the ground
    // (docs/design/cover.md). Rock is always the tallest value, 255, which blocks everything.
    uint8_t obstacleDm(CellPos c) const {
        if (c.level == 0) [[likely]] {
            return terrain_[static_cast<size_t>(index(c))] == Terrain::Rock ? uint8_t{255}
                                                                            : obstacle_[static_cast<size_t>(index(c))];
        }
        const UpperChunk* chunk = upperChunk(c);
        return chunk != nullptr ? chunk->obstacle[upperSlot(c)] : uint8_t{0};
    }

    // Raw passage layer of the ground, indexed by `index`, for the pathfinder's inner loop. Upper
    // levels have no whole layer; there the pathfinder asks the regions which cells are doors.
    const Passage* groundPassage() const { return passage_.data(); }
    // Counts up every time a building is set or cleared in any cell, so a system that reads the
    // building layer can tell its picture is stale (Rooms).
    uint64_t buildingChanges() const { return buildingChanges_; }

    // Steam geysers (ground level): a geothermal generator over one makes power (docs/design/power.md).
    // Map generation (S8) places them from the seed; they are not saved, so whoever places them must do
    // it again the same way after a load.
    void setGeyser(CellPos c, bool present);
    // Moving water (ground level): water cells of a river. A watermill is built on these and nowhere
    // else, and a moisture pump leaves them alone. Like geysers, map generation (S8) places them and
    // they are not saved.
    void setFlowing(CellPos c, bool present);
    bool flowing(CellPos c) const {
        return c.level == 0 && inBounds(c) && !flowing_.empty() && flowing_[static_cast<size_t>(index(c))] != 0 &&
               terrain(c) == Terrain::Water;
    }
    // Where a building can stand: walkable ground, or for a building made for moving water, moving water.
    bool groundBuildable(CellPos c, bool onFlowingWater) const {
        return onFlowingWater ? flowing(c) : groundWalkable(c);
    }
    // A moisture pump turns a water cell into soil. The cells it dried are kept (in order), because the
    // terrain is made again from the seed on loading and `restoreDried` puts them back.
    void dryWater(CellPos c);
    const std::vector<uint32_t>& dried() const { return dried_; }
    void restoreDried(const std::vector<uint32_t>& cells);
    bool geyser(CellPos c) const { return c.level == 0 && inBounds(c) && !geyser_.empty() && geyser_[static_cast<size_t>(index(c))] != 0; }

    // The roof over a cell. Thick over the rock the map was generated with (and over it for good,
    // even once mined out); Thin where Rooms roofed an enclosed room or where the cell above has an
    // upper floor; else None. The cell must be in bounds.
    Roof roof(CellPos c) const {
        Roof stored = Roof::None;
        if (c.level == 0) [[likely]] {
            stored = roof_[static_cast<size_t>(index(c))];
        } else if (const UpperChunk* chunk = upperChunk(c)) {
            stored = chunk->roof[upperSlot(c)];
        }
        return stored == Roof::None && floorAbove(c) ? Roof::Thin : stored;
    }

    // The mountain's thick roof over a ground cell (M3, docs/design/roofs.md): generation puts it over
    // all rock; it stays when the rock is mined out, held up by rock and walls within the roof support
    // radius, and RoofCollapse takes it away when nothing holds it. `false` clears it.
    void setThickRoof(CellPos c, bool thick);
    // Ground cells (by `index`) that are no longer rock but still have the thick roof: the only thick
    // roofs that can fall. In the order they were mined out.
    const std::vector<int>& hollowRoofCells() const { return hollow_; }
    // Goes up whenever rock is mined out under a thick roof or a thick roof is cleared.
    uint64_t rockChanges() const { return rockChanges_; }

    // Regions and reachability groups of the walkable cells (docs/design/regions.md).
    Regions& regions();
    // Cover from the eight sides and line of sight (docs/design/cover.md).
    Cover& cover();

    // Hash of every layer, for determinism tests.
    uint64_t contentHash() const;

private:
    friend class Rooms;  // the only writer of the thin roof

    // The layers of one 16 m chunk of an upper level, indexed by upperSlot.
    struct UpperChunk {
        static constexpr int kCells = kUpperChunk * kUpperChunk;
        UpperChunk() { building.fill(kNoEntity); }
        std::array<uint8_t, kCells> floor{};  // 1 where an upper floor is laid
        std::array<Entity, kCells> building;
        std::array<Passage, kCells> passage{};
        std::array<uint8_t, kCells> obstacle{};
        std::array<Roof, kCells> roof{};
    };

    // The chunk holding an upper-level cell, or null if nothing was ever built there.
    const UpperChunk* upperChunk(CellPos c) const {
        if (upper_.empty()) {
            return nullptr;
        }
        return upper_[upperChunkIndex(c)].get();
    }
    UpperChunk& upperChunkFor(CellPos c);
    size_t upperChunkIndex(CellPos c) const {
        return (static_cast<size_t>(c.level - 1) * static_cast<size_t>(chunksY_) + static_cast<size_t>(c.y / kUpperChunk)) *
                   static_cast<size_t>(chunksX_) +
               static_cast<size_t>(c.x / kUpperChunk);
    }
    static size_t upperSlot(CellPos c) {
        return static_cast<size_t>((c.y % kUpperChunk) * kUpperChunk + c.x % kUpperChunk);
    }
    // The stored roof of a cell, for Rooms to write.
    Roof& roofSlot(CellPos c);

    void generate();
    void carveRivers();
    void addWetlands();

    int width_;
    int height_;
    uint64_t seed_;
    std::vector<float> groundHeight_;
    std::vector<Terrain> terrain_;
    std::vector<Entity> building_;
    std::vector<Passage> passage_;
    std::vector<uint8_t> obstacle_;
    std::vector<Roof> roof_;
    int chunksX_ = 0;
    int chunksY_ = 0;
    // Upper-level chunks, level 1 first then row order; empty until the first is needed, and a
    // chunk is null until something is built in it.
    std::vector<std::unique_ptr<UpperChunk>> upper_;
    uint64_t upperFloorChanges_ = 0;
    uint64_t buildingChanges_ = 0;
    uint64_t rockChanges_ = 0;
    std::vector<uint8_t> geyser_;  // 1 where a steam geyser vents
    std::vector<uint8_t> flowing_;  // 1 where the water moves (a river)
    std::vector<uint32_t> dried_;   // cells a moisture pump dried, in the order it did
    std::vector<int> hollow_;  // mined-out ground cells that still have the thick roof
    std::vector<LevelLink> levelLinks_;
    std::unique_ptr<Regions> regions_;
    std::unique_ptr<Cover> cover_;
    float river_;
    float marsh_;
};

}  // namespace colony
