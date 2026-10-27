#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Random.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace colony {

class Map;
class StateHasher;

// A cell with deep resources: which item lies under it and how many units are left.
struct DeepCell {
    int cell = 0;  // Map::index
    DefId def;
    uint16_t units = 0;
};

// Work a scanner or drill has done towards its next find or portion, by the building's origin cell.
struct DeepWork {
    CellPos at;
    double work = 0.0;
};

// What is saved (section "deep").
struct DeepResourcesSaved {
    std::vector<DeepCell> cells;
    std::vector<DeepWork> work;
    uint64_t rngState = 0;
    uint64_t rngIncrement = 1;
    uint64_t found = 0;
    uint64_t portions = 0;
};

// A deposit the scanner (or the long-range scanner hook) just made.
struct DeepFind {
    DefId def;
    CellPos center;
    int cells = 0;
};

// What one drill portion gave: `count` of `def` (before the miner's yield), taken from `from`; `chunk` when
// there was nothing under the drill and it only brought up a bit of rock.
struct DeepPortion {
    DefId def;
    int count = 0;
    CellPos from;
    bool chunk = false;
};

// Deep resources (docs/design/deep-mining.md). Under the ground lie deposits no pick can reach; the
// ground-penetrating scanner finds them (a find makes the deposit: nothing is there before) and the deep
// drill takes what is under its radius, a portion at a time. Items with a `deep` object in their def can
// be deposits: `deep.lumpSize` [min, max] cells, `deep.commonality` and `deep.portion` per drill cycle.
class COLONY_API DeepResources {
public:
    DeepResources(Map& map, const DefDatabase& defs, uint64_t seed);

    bool has(CellPos c) const;
    DefId defAt(CellPos c) const;
    int unitsAt(CellPos c) const;
    size_t cellCount() const { return cells_.size(); }
    int totalUnits() const;
    // Bumped on every change of the layer, so the snapshot only copies it when it moved.
    uint64_t changes() const { return changes_; }
    uint64_t found() const { return found_; }
    uint64_t portions() const { return portions_; }

    // The item defs that can be deposits, in def order.
    const std::vector<DefId>& kinds() const { return kinds_; }
    // Units a drill portion of this item takes out of one cell; 0 if it is not a deposit item.
    int portionOf(DefId def) const;

    // The rules (data/defs/Rules/DeepRules.json).
    double unitsPerCell() const { return unitsPerCell_; }
    double drillCycleSeconds() const { return drillCycleSeconds_; }
    double scanAverageSeconds() const { return scanAverageSeconds_; }
    double scanGuaranteedSeconds() const { return scanGuaranteedSeconds_; }
    double scanSpeedBase() const { return scanSpeedBase_; }
    double scanSpeedPerLevel() const { return scanSpeedPerLevel_; }
    int defaultSkillLevel() const { return defaultSkillLevel_; }
    double scanXpPerSecond() const { return scanXpPerSecond_; }
    double drillXpPerSecond() const { return drillXpPerSecond_; }

    // Makes a new deposit somewhere on the ground (level 0, not under water), of `def` or, if that is
    // invalid, a kind chosen by commonality. Its size is the def's lump size; it grows over cells with
    // ground and goes round the rest. This is the hook the ground-penetrating scanner uses, and what a
    // long-range mineral scanner, an incident or a quest calls to put resources on the map. Returns false
    // when the map has no room or no deposit kind exists.
    bool findDeposit(DefId def, DeepFind& out);
    // The same at a chosen cell (the deposit grows from there). For tests and scenarios.
    bool placeDeposit(DefId def, CellPos center, int cells, DeepFind* out = nullptr);

    // One game second of scanner work by the scanner whose origin cell is `building`: `speed` is the
    // share of a full-speed second (the operator's research speed). Each second has speed / average
    // chance of a find (the find time is exponential, as RimWorld's), and work past the guaranteed time
    // always finds. A find resets the scanner's work. Returns true on a find (`out` says what).
    bool scanStep(CellPos building, double speed, DeepFind& out);
    // Work done so far by that scanner or drill.
    double workAt(CellPos building) const;

    // Whether the drill at `building` has deep resources in its radius.
    bool hasResourceNear(CellPos building, double radius) const;
    // Drill work: adds `amount` seconds of work at full speed; returns true when a whole cycle is done
    // (the work then goes back down by one cycle).
    bool drillStep(CellPos building, double amount);
    // Takes one portion out of the nearest cell with something in the radius (the nearest first, then by
    // row). With nothing there, `chunk` is set and `def` is invalid: the caller brings up a rock chunk.
    DeepPortion takePortion(CellPos building, double radius);
    // Forgets a building's work (it was taken down).
    void forgetWork(CellPos building);

    void hashState(StateHasher& hasher) const;
    DeepResourcesSaved saved() const;
    // False for a cell off the map, repeated, or naming something that is not a deposit item.
    bool restore(const DeepResourcesSaved& state);

private:
    struct Kind {
        DefId def;
        int lumpMin = 1;
        int lumpMax = 1;
        double commonality = 1.0;
        int portion = 10;
    };

    const Kind* kindOf(DefId def) const;
    DeepWork* workEntry(CellPos building, bool create);
    bool buildable(CellPos c) const;

    Map& map_;
    std::vector<Kind> info_;
    std::vector<DefId> kinds_;
    double unitsPerCell_ = 300.0;
    double drillCycleSeconds_ = 20160.0;
    double scanAverageSeconds_ = 259200.0;
    double scanGuaranteedSeconds_ = 518400.0;
    double scanSpeedBase_ = 0.2;
    double scanSpeedPerLevel_ = 0.1;
    int defaultSkillLevel_ = 8;
    double scanXpPerSecond_ = 0.0486;
    double drillXpPerSecond_ = 0.048611;
    std::unordered_map<int, size_t> index_;  // Map::index -> position in cells_
    std::vector<DeepCell> cells_;
    std::vector<DeepWork> work_;
    Rng rng_;
    uint64_t changes_ = 0;
    uint64_t found_ = 0;
    uint64_t portions_ = 0;
};

}  // namespace colony
