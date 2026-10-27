#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Grid.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace colony {

class Map;

// One mineable cell as saved: its index in the map, the def of the rock or ore and the hit
// points it has left.
struct MineableRecord {
    int cell = 0;
    DefId def;
    uint16_t hitPoints = 0;
};

// What mining a cell gives, before the miner's skill: `count` of `def`, or nothing (an invalid def)
// when a rock leaves no chunk.
struct MineDrop {
    DefId def;
    int count = 0;
};

// The rock layer: for every cell of Terrain::Rock, which rock or ore it is and how many hit points
// it has left (docs/design/mining.md). Made from the Things defs with category "Mineable" when
// the map is made; mining it down turns the cell into Terrain::RoughStone.
//
// Flat arrays like the plant layer, because a 1 km map has hundreds of thousands of rock cells.
class COLONY_API Mineables {
public:
    // Fills the layer for a new map: 2 or 3 of the rock types (chosen from the seed) in patches,
    // and ore lumps inside the rock. With no Mineable defs the layer is empty and rock stays
    // solid for good. `seed` makes it repeatable.
    Mineables(Map& map, const DefDatabase& defs, uint64_t seed);

    bool has(CellPos c) const { return map_inBounds(c) && kind_[indexOf(c)] != 0; }
    // The def of the rock or ore in the cell, or an invalid DefId.
    DefId defAt(CellPos c) const;
    int hitPoints(CellPos c) const { return map_inBounds(c) ? hp_[indexOf(c)] : 0; }
    // Full hit points of a mineable def; 0 if it is not one.
    int maxHitPoints(DefId def) const;
    bool isOre(DefId def) const;
    size_t count() const { return count_; }
    size_t countOf(DefId def) const;
    // Mineable defs, in def order (rock types first or not, as the defs are).
    const std::vector<DefId>& defs() const { return defIds_; }
    // The rock types this map was given.
    const std::vector<DefId>& rockTypes() const { return rockTypes_; }

    // Puts rock or ore in a cell (its terrain becomes Rock), at full hit points. Returns false
    // for a def that is not mineable or a cell off the map. For tests and map generation; the
    // game itself never builds rock.
    bool place(CellPos c, DefId def);

    // Deals damage. When the hit points reach 0 the cell is mined: the layer forgets it and the
    // terrain becomes RoughStone (regions and cover are told). Returns true if it was mined.
    // Does nothing, returning false, in a cell with no rock.
    bool hit(CellPos c, int damage);

    // What mining this cell gives: ore its full count; a rock its chunk with the def's
    // `dropChance`, decided by a hash of the map seed and cell, so it needs no random stream and
    // doesn't depend on who mines it when.
    MineDrop dropOf(CellPos c) const;

    // A rock chunk of one of this map's rock types, chosen by a hash of the cell (what a deep drill with nothing
    // under it brings up); an invalid DefId when the map has no rock type with a chunk.
    DefId chunkOf(CellPos c) const;

    // Fingerprint of the layer; kept up to date as it changes, so asking costs nothing.
    uint64_t hash() const { return hash_ ^ static_cast<uint64_t>(count_); }

    // For saves: every mineable cell in cell order, and a way to put them back. Restoring also
    // makes the terrain agree: Rock where there is a record, RoughStone for rock cells with none
    // (they were mined). Returns false (leaving it empty) on a record off the map, repeated or
    // naming something that is not mineable.
    std::vector<MineableRecord> records() const;
    bool restore(const std::vector<MineableRecord>& records);

private:
    struct Kind {
        DefId def;
        int hitPoints = 0;
        DefId dropDef;
        int dropCount = 1;
        double dropChance = 1.0;
        bool isOre = false;
        double commonality = 1.0;
        int lumpMin = 1;
        int lumpMax = 1;
    };

    bool map_inBounds(CellPos c) const;
    size_t indexOf(CellPos c) const;
    int kindOfDef(DefId def) const;
    uint64_t cellHash(size_t i) const;
    void set(size_t i, uint8_t kind, uint16_t hp);
    void generate(const DefDatabase& defs);

    Map& map_;
    uint64_t seed_;
    std::vector<Kind> kinds_;
    std::vector<DefId> defIds_;
    std::vector<DefId> rockTypes_;
    std::vector<int> kindOfDef_;  // by DefId.value: index into kinds_, or -1
    std::vector<uint8_t> kind_;   // 0: none, otherwise index into kinds_ plus 1
    std::vector<uint16_t> hp_;
    size_t count_ = 0;
    uint64_t hash_ = 0;
};

}  // namespace colony
