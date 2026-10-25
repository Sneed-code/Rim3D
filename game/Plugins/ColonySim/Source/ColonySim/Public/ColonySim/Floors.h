#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Grid.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace colony {

class Map;
class StateHasher;

// The floors laid on the ground (docs/design/room-stats.md): at most one per cell, a Terrain def
// from data/defs/Terrain/Floors.json (wood floor, stone tiles...). A floor sets the cell's beauty
// and cleanliness in place of the bare ground's, and is the reason dirt can be dropped: pawns pick
// dirt up on bare ground and drop it on floors.
//
// Floors are not things. Laying them is the PlaceFloorCommand for now (instant); a work job comes
// with the architect's floor tool.
class COLONY_API Floors {
public:
    Floors(const Map& map, const DefDatabase& defs);

    // The floor on a cell, or an invalid DefId.
    DefId at(CellPos c) const;
    // Lays a floor (replacing any), or takes it off with an invalid DefId. False if the cell is off
    // the map or is water or rock, or `floor` is not a floor def.
    bool set(CellPos c, DefId floor);
    bool isFloorDef(DefId def) const;

    size_t count() const { return floors_.size(); }
    // Every floor as (cell index, def), in cell order.
    const std::map<int, DefId>& all() const { return floors_; }
    // Goes up whenever a floor is laid or taken off.
    uint64_t changes() const { return changes_; }

    void hashState(StateHasher& h) const;
    // Replaces every floor (loading a save). False, leaving none, on a cell off the map or a def
    // that is not a floor.
    bool restore(const std::vector<std::pair<int, DefId>>& floors);

private:
    const Map& map_;
    const DefDatabase& defs_;
    std::map<int, DefId> floors_;
    uint64_t changes_ = 0;
};

}  // namespace colony
