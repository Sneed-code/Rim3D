#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Grid.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace colony {

class Map;

// Regions and reachability, as RimWorld's regions. See docs/design/regions.md.
//
// Walkable cells are grouped into regions: 4-connected areas that never cross a 16 m block, so a
// changed cell rebuilds only its own block. A door cell is a region of its own. Regions that touch
// are linked, and so are the regions at the two ends of stairs or a ladder, whatever their level
// (docs/design/levels.md). Linked regions share a reachability group, which answers "can a pawn walk from
// here to there?" with two lookups.

using RegionId = uint32_t;
inline constexpr RegionId kNoRegion = 0xFFFFFFFFu;
inline constexpr uint32_t kNoGroup = 0xFFFFFFFFu;

enum class RegionKind : uint8_t { Normal, Door };

// Two regions that share a border, or that stairs or a ladder join across levels (Map::levelLinks).
// Both regions hold a link to each other with the same numbers.
struct RegionLink {
    RegionId other = kNoRegion;
    int faces = 0;     // cell sides the two regions share, or stairs and ladders between them
    float x = 0.0f;    // middle of the shared border, metres; for a climb, halfway between its ends
    float y = 0.0f;
    float extra = 0.0f;  // metres a path pays on top of the flat walk: the cheapest climb's extra
};

struct Region {
    RegionKind kind = RegionKind::Normal;
    bool alive = false;  // false for a free id
    int cellCount = 0;
    CellRect bounds;     // smallest rectangle around the region's cells, on its level
    CellPos anchor;      // the region's first cell in row order; always one of its cells
    uint32_t group = kNoGroup;
    bool climbs = false;  // some stairs or ladder starts or ends in it (the pathfinder looks)
    std::vector<RegionLink> links;  // sorted by `other`
};

class COLONY_API Regions {
public:
    static constexpr int kBlockSize = 16;  // metres; a region never crosses a block boundary

    // Nothing is built until the first query, so maps used only for terrain cost nothing.
    explicit Regions(const Map& map);

    // Queries rebuild dirty blocks first, so they always see the map as it is now.

    // The region a cell belongs to; kNoRegion if the cell is off the map or can't be walked.
    RegionId regionAt(CellPos c);
    // The reachability group of a cell; kNoGroup if it has no region.
    uint32_t groupAt(CellPos c);
    // True if a pawn can walk from `from` to `to`: both have regions in the same group.
    bool canReach(CellPos from, CellPos to);

    // The region of a cell as of the last rebuild, without rebuilding: for hot loops (the
    // pathfinder) that call update() first. kNoRegion off the map or on a cell that can't be walked.
    RegionId regionAtNow(CellPos c) const;
    // The same for a whole level at once, indexed by Map::index; for the pathfinder's inner loop.
    // Null for an upper level that has never had anything built on it (no cell there has a region).
    const RegionId* regionIdsOnLevel(int level) const;
    // One more than the highest region id ever used; arrays indexed by region id need this size.
    size_t idLimit() const { return regions_.size(); }
    // Goes up every time a cell's passage changes, so a caller can tell its region ids are stale.
    uint64_t changeCount() const { return changeCount_; }

    // A region by id, from a query since the last change. Does not rebuild, so the reference
    // stays valid until the map changes and something queries again.
    const Region& region(RegionId id) const { return regions_[id]; }
    // Live regions in one block, in id order.
    const std::vector<RegionId>& regionsInBlock(int blockX, int blockY, int level);
    size_t regionCount();
    size_t groupCount();
    // Walkable cells in a group.
    int groupCellCount(uint32_t group);

    int blocksX() const { return blocksX_; }
    int blocksY() const { return blocksY_; }

    // The map calls this when a cell changes between walkable, door and blocked.
    void markDirty(CellPos c);
    // Rebuilds dirty blocks now. Queries do this themselves.
    void update();

    // Counters for tests and simrun.
    uint64_t blocksRebuilt() const { return blocksRebuilt_; }
    uint64_t groupRelabels() const { return groupRelabels_; }

private:
    int blockOf(int x, int y, int level) const { return (level * blocksY_ + y / kBlockSize) * blocksX_ + x / kBlockSize; }
    // The region of an in-bounds cell, kNoRegion on an upper level with nothing stored yet.
    RegionId regionOfCell(CellPos c) const {
        const std::vector<RegionId>& level = regionOf_[static_cast<size_t>(c.level)];
        return level.empty() ? kNoRegion : level[static_cast<size_t>(cellIndex(c))];
    }
    // The slot of an in-bounds cell, making its level's layer first if it has none.
    RegionId& regionSlot(CellPos c);
    int cellIndex(CellPos c) const;
    void rebuildBlocks();
    void removeRegion(RegionId id);
    RegionId newRegion();
    void floodBlock(int block);
    void linkBlock(int block);
    void linkLevels();
    void relabelGroups();

    const Map& map_;
    int blocksX_ = 0;
    int blocksY_ = 0;
    int blocks_ = 0;
    bool built_ = false;
    // Per level, per cell; level 0 from the start, an upper level once something is built on it.
    std::vector<std::vector<RegionId>> regionOf_;
    std::vector<Region> regions_;               // indexed by RegionId
    std::vector<RegionId> freeIds_;
    std::vector<std::vector<RegionId>> blockRegions_;
    std::vector<uint8_t> blockDirty_;
    std::vector<int> dirtyBlocks_;
    std::vector<int> groupCells_;               // walkable cells per group
    size_t liveRegions_ = 0;
    uint64_t blocksRebuilt_ = 0;
    uint64_t groupRelabels_ = 0;
    uint64_t changeCount_ = 0;

    // Scratch, kept between rebuilds to avoid allocations.
    std::vector<int> floodQueue_;
    struct PendingLink {
        RegionId a;
        RegionId b;
        float x;
        float y;
        float extra;
    };
    std::vector<PendingLink> pendingLinks_;
};

}  // namespace colony
