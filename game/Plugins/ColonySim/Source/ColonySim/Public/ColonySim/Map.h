#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace colony {

class Cover;
class Regions;
class Rooms;

// Placeholder terrain types until terrain defs are loaded from data/defs.
enum class Terrain : uint8_t { Water, Sand, Soil, Rock };

// How the building in a cell affects movement. A door can be walked through but is a region of
// its own (docs/design/regions.md).
enum class Passage : uint8_t { Open, Blocked, Door };

// The roof over a cell. Rock is the mountain's own roof (thick) and stays when a cell is mined
// out; Thin is the roof that Rooms puts over enclosed rooms (docs/design/rooms.md).
enum class Roof : uint8_t { None, Thin, Thick };

struct MapSettings {
    int width = 512;   // cells, 1 m each
    int height = 512;  // cells, 1 m each
    uint64_t seed = 0;
    bool flat = false;  // every cell soil at height 0, for tests
};

// The map grid: one flat array per layer, generated from a seed.
//
// Only the ground level (0) is stored so far. Cells on other levels are out of bounds until
// upper floors and stairs come (vanilla core); the interfaces already take the level.
//
// The map owns its regions and tells them when a cell's passage changes. It can't be copied or
// moved, because its regions point back at it.
class COLONY_API Map {
public:
    explicit Map(const MapSettings& settings);
    ~Map();
    Map(const Map&) = delete;
    Map& operator=(const Map&) = delete;

    int width() const { return width_; }
    int height() const { return height_; }
    int levels() const { return 1; }
    bool inBounds(CellPos c) const { return c.level == 0 && c.x >= 0 && c.y >= 0 && c.x < width_ && c.y < height_; }
    // Index of the cell within its level's layers. The cell must be in bounds.
    int index(CellPos c) const { return c.y * width_ + c.x; }
    CellPos cellAt(int index, int level = 0) const { return CellPos{index % width_, index / width_, level}; }

    // Placeholder until terrain defs: water and rock can't be walked or built on.
    bool groundWalkable(CellPos c) const {
        const Terrain t = terrain(c);
        return t != Terrain::Water && t != Terrain::Rock;
    }
    // Pawns can stand here: in bounds, walkable ground and no building that blocks movement.
    bool walkable(CellPos c) const {
        return inBounds(c) && groundWalkable(c) && passage_[static_cast<size_t>(index(c))] != Passage::Blocked;
    }
    // A walkable cell with a door in it.
    bool isDoor(CellPos c) const {
        return walkable(c) && passage_[static_cast<size_t>(index(c))] == Passage::Door;
    }

    // Ground height in metres. The cell must be in bounds.
    float groundHeight(CellPos c) const { return groundHeight_[static_cast<size_t>(index(c))]; }
    Terrain terrain(CellPos c) const { return terrain_[static_cast<size_t>(index(c))]; }
    // Ground height in metres at any point, interpolated between the heights of the four grid
    // corners around it. The corner (x, y) is the point x metres by y metres, which is how the
    // terrain mesh is drawn, so pawns stand on the surface the player sees. Clamped to the map.
    float heightAt(float x, float y) const;
    // Ground-column shorthands for code that only reads the ground surface (the terrain mesh).
    float groundHeight(int x, int y) const { return groundHeight(CellPos{x, y, 0}); }
    Terrain terrain(int x, int y) const { return terrain(CellPos{x, y, 0}); }

    // Building layer: the building covering each cell, kNoEntity if none. Kept up to date by
    // Things when buildings spawn and despawn; nothing else should call the setters.
    Entity buildingAt(CellPos c) const { return building_[static_cast<size_t>(index(c))]; }
    // `obstacleDm` is how tall the building is for sight and cover (see Cover.h); 0 for a
    // building that is neither.
    void setBuilding(CellPos c, Entity building, Passage passage, uint8_t obstacleDm = 0);
    void clearBuilding(CellPos c);
    // Changes only the height for sight and cover of the building in a cell; a door does it when it
    // opens (it stops blocking sight) and closes (docs/design/rooms.md).
    void setObstacle(CellPos c, uint8_t obstacleDm);
    Passage passage(CellPos c) const { return passage_[static_cast<size_t>(index(c))]; }
    // How tall what stands in the cell is for sight and cover, in decimetres above the ground
    // (docs/design/cover.md). Rock is always the tallest value, 255, which blocks everything.
    uint8_t obstacleDm(CellPos c) const {
        return terrain(c) == Terrain::Rock ? uint8_t{255} : obstacle_[static_cast<size_t>(index(c))];
    }

    // Raw passage layer of a level, indexed by `index`, for the pathfinder's inner loop.
    const Passage* passageOnLevel(int) const { return passage_.data(); }
    // Counts up every time a building is set or cleared in any cell, so a system that reads the
    // building layer can tell its picture is stale (Rooms).
    uint64_t buildingChanges() const { return buildingChanges_; }

    // The roof over a cell. Thick over the rock the map was generated with (and over it for good,
    // even once mined out), Thin where Rooms roofed an enclosed room, else None. The cell must be
    // in bounds.
    Roof roof(CellPos c) const { return roof_[static_cast<size_t>(index(c))]; }

    // Regions and reachability groups of the walkable cells (docs/design/regions.md).
    Regions& regions();
    // Cover from the eight sides and line of sight (docs/design/cover.md).
    Cover& cover();

    // Hash of every layer, for determinism tests.
    uint64_t contentHash() const;

private:
    friend class Rooms;  // the only writer of the thin roof

    void generate();

    int width_;
    int height_;
    uint64_t seed_;
    std::vector<float> groundHeight_;
    std::vector<Terrain> terrain_;
    std::vector<Entity> building_;
    std::vector<Passage> passage_;
    std::vector<uint8_t> obstacle_;
    std::vector<Roof> roof_;
    uint64_t buildingChanges_ = 0;
    std::unique_ptr<Regions> regions_;
    std::unique_ptr<Cover> cover_;
};

}  // namespace colony
