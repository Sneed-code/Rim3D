#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Grid.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace colony {

class Map;
class StateHasher;

// What the player has marked a cell for, as RimWorld's designations. Workers pick these up through
// their work scanners (docs/design/mining.md). New kinds go at the end: saves store the number.
enum class DesignationKind : uint8_t {
    None,
    Mine,          // dig out the rock or ore in the cell
    CutPlant,      // fell the tree or cut down the plant; a tree gives wood
    HarvestPlant,  // pick what the plant gives (berries, wood from a tree)
};

COLONY_API const char* designationKindName(DesignationKind kind);

// The designations on the map, at most one per cell. Kept in cell order so scanners and saves
// see them in the same order every time.
class COLONY_API Designations {
public:
    explicit Designations(const Map& map) : map_(map) {}

    DesignationKind at(CellPos c) const;
    // Marks a cell (replacing any other mark), or clears it with None. False if the cell is off
    // the map. Whether the cell suits the kind is the caller's business (Sim checks).
    bool set(CellPos c, DesignationKind kind);
    size_t count() const { return marks_.size(); }
    size_t countOf(DesignationKind kind) const;

    // Every mark as (cell index, kind), in cell order.
    const std::map<int, DesignationKind>& all() const { return marks_; }

    void hashState(StateHasher& h) const;

    // Replaces all marks (loading a save). False, leaving none, if a cell is off the map or a
    // kind is None.
    bool restore(const std::vector<std::pair<int, DesignationKind>>& marks);

private:
    const Map& map_;
    std::map<int, DesignationKind> marks_;
};

}  // namespace colony
