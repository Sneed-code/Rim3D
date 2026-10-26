#include "ColonySim/UpperFloors.h"

#include "ColonySim/Map.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Things.h"

namespace colony {

UpperFloors::UpperFloors(Map& map, Rooms& rooms, Things& things) : map_(map), rooms_(rooms), things_(things) {}

std::string UpperFloors::whyNotLay(CellPos c) const {
    if (!map_.inBounds(c) || c.level < 1) {
        return "upper floors go on levels 1 to " + std::to_string(Map::kLevels - 1) + " of the map";
    }
    if (map_.upperFloor(c)) {
        return {};
    }
    const CellPos below{c.x, c.y, c.level - 1};
    if (map_.roof(below) == Roof::Thick) {
        return "the mountain is in the way";
    }
    // A stairwell (U17): stairs and ladders climb up through the cells over them.
    const entt::registry& reg = things_.registry();
    if (const Entity building = map_.buildingAt(below);
        building != kNoEntity && reg.all_of<Thing>(building) && things_.defs().get(reg.get<Thing>(building).def).has("building.climb")) {
        return "the stairs go up through there";
    }
    if (!rooms_.roofSupported(below)) {
        return "nothing holds it up: it needs a wall or column within " +
               std::to_string(static_cast<int>(rooms_.numbers().roofSupportRadius)) + " m on the level below";
    }
    return {};
}

std::string UpperFloors::whyNotRemove(CellPos c) const {
    if (!map_.upperFloor(c)) {
        return {};
    }
    if (map_.buildingAt(c) != kNoEntity) {
        return "a building stands on it";
    }
    if (!things_.itemsAt(c).empty()) {
        return "something lies on it";
    }
    const entt::registry& reg = things_.registry();
    for (const Entity pawn : reg.view<const Position, const Mover>()) {
        if (cellOf(reg.get<Position>(pawn)) == c) {
            return "someone stands on it";
        }
    }
    return {};
}

bool UpperFloors::lay(CellPos c, std::string* why) {
    std::string reason = whyNotLay(c);
    if (!reason.empty()) {
        if (why != nullptr) {
            *why = std::move(reason);
        }
        return false;
    }
    return map_.setUpperFloor(c, true);
}

bool UpperFloors::remove(CellPos c, std::string* why) {
    std::string reason = whyNotRemove(c);
    if (!reason.empty()) {
        if (why != nullptr) {
            *why = std::move(reason);
        }
        return false;
    }
    return !map_.inBounds(c) || c.level < 1 || map_.setUpperFloor(c, false);
}

}  // namespace colony
