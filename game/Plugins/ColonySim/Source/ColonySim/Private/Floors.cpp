#include "ColonySim/Floors.h"

#include "ColonySim/Hash.h"
#include "ColonySim/Map.h"

namespace colony {

Floors::Floors(const Map& map, const DefDatabase& defs) : map_(map), defs_(defs) {}

DefId Floors::at(CellPos c) const {
    if (!map_.onGround(c)) {
        return DefId{};
    }
    const auto it = floors_.find(map_.index(c));
    return it == floors_.end() ? DefId{} : it->second;
}

bool Floors::isFloorDef(DefId def) const {
    return def.valid() && def.value < defs_.size() && defs_.get(def).kind() == "Terrain" &&
           defs_.get(def).text("designationCategory") == "Floors";
}

bool Floors::set(CellPos c, DefId floor) {
    if (!map_.onGround(c) || !map_.groundWalkable(c)) {
        return false;
    }
    const int index = map_.index(c);
    if (!floor.valid()) {
        if (floors_.erase(index) > 0) {
            ++changes_;
        }
        return true;
    }
    if (!isFloorDef(floor)) {
        return false;
    }
    const auto it = floors_.find(index);
    if (it == floors_.end() || it->second != floor) {
        floors_[index] = floor;
        ++changes_;
    }
    return true;
}

void Floors::hashState(StateHasher& h) const {
    h.add(static_cast<uint64_t>(floors_.size()));
    for (const auto& [index, def] : floors_) {
        h.add(index);
        h.add(def.value);
    }
}

bool Floors::restore(const std::vector<std::pair<int, DefId>>& floors) {
    floors_.clear();
    ++changes_;
    for (const auto& [index, def] : floors) {
        if (index < 0 || index >= map_.width() * map_.height() || !isFloorDef(def)) {
            floors_.clear();
            return false;
        }
        floors_[index] = def;
    }
    return true;
}

}  // namespace colony
