#include "ColonySim/Windows.h"

#include "ColonySim/Construction.h"
#include "ColonySim/Cover.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Things.h"

#include <algorithm>

namespace colony {

WindowNumbers WindowNumbers::read(const DefDatabase& defs) {
    WindowNumbers n;
    const Def* def = defs.find("Rules", "WindowRules");
    if (def == nullptr) {
        return n;
    }
    n.sillDm = static_cast<uint8_t>(std::clamp(def->number("sillDm", n.sillDm), 1.0, 200.0));
    n.lintelDm = static_cast<uint8_t>(std::clamp(def->number("lintelDm", n.lintelDm), static_cast<double>(n.sillDm) + 1.0, 254.0));
    n.lightRadius = static_cast<float>(std::max(0.0, def->number("lightRadius", n.lightRadius)));
    n.lightGlow = static_cast<float>(std::clamp(def->number("lightGlow", n.lightGlow), 0.0, 1.0));
    n.addWork = std::max(1.0, def->number("addWork", n.addWork));
    n.removeWork = std::max(1.0, def->number("removeWork", n.removeWork));
    return n;
}

BuildTool buildToolOf(const DefDatabase& defs, const std::string& defName) {
    const Def* def = defs.find("Things", defName);
    if (def == nullptr || def->text("category") != "BuildTool") {
        return BuildTool::None;
    }
    const std::string tool = def->text("buildTool");
    if (tool == "AddWindow") {
        return BuildTool::AddWindow;
    }
    if (tool == "RemoveWindow") {
        return BuildTool::RemoveWindow;
    }
    return BuildTool::None;
}

Windows::Windows(Map& map, Things& things, const DefDatabase& defs)
    : map_(map), things_(things), defs_(defs), numbers_(WindowNumbers::read(defs)) {}

Entity Windows::wallAt(CellPos c) const {
    if (!map_.inBounds(c)) {
        return kNoEntity;
    }
    const Entity wall = map_.buildingAt(c);
    const entt::registry& reg = things_.registry();
    if (wall == kNoEntity || !reg.valid(wall) || reg.all_of<ConstructionSite>(wall)) {
        return kNoEntity;
    }
    const Thing* thing = reg.try_get<Thing>(wall);
    const Footprint* footprint = reg.try_get<Footprint>(wall);
    if (thing == nullptr || footprint == nullptr || footprint->sizeX != 1 || footprint->sizeY != 1 ||
        !defs_.get(thing->def).flag("building.takesWindow")) {
        return kNoEntity;
    }
    return wall;
}

const Window* Windows::at(CellPos c) const {
    const Entity wall = wallAt(c);
    return wall != kNoEntity ? things_.registry().try_get<Window>(wall) : nullptr;
}

std::string Windows::whyNotAdd(CellPos c) const {
    if (!map_.inBounds(c)) {
        return "off the map";
    }
    if (wallAt(c) == kNoEntity) {
        return "windows go in finished walls";
    }
    const Window* window = at(c);
    if (window != nullptr && !window->broken) {
        return "it has a window already";
    }
    return {};
}

std::string Windows::whyNotRemove(CellPos c) const {
    if (!map_.inBounds(c)) {
        return "off the map";
    }
    return at(c) == nullptr ? "there is no window there" : std::string();
}

bool Windows::add(CellPos c) {
    if (!whyNotAdd(c).empty()) {
        return false;
    }
    const Entity wall = wallAt(c);
    things_.registry().emplace_or_replace<Window>(wall, Window{false});
    apply(wall);
    return true;
}

bool Windows::remove(CellPos c) {
    const Entity wall = wallAt(c);
    if (wall == kNoEntity || !things_.registry().all_of<Window>(wall)) {
        return false;
    }
    things_.registry().remove<Window>(wall);
    apply(wall);
    return true;
}

bool Windows::breakGlass(Things& things, Entity wall) {
    entt::registry& reg = things.registry();
    Window* window = reg.valid(wall) ? reg.try_get<Window>(wall) : nullptr;
    if (window == nullptr || window->broken) {
        return false;
    }
    window->broken = true;
    return true;
}

void Windows::apply(Entity wall) {
    entt::registry& reg = things_.registry();
    Footprint* footprint = reg.valid(wall) ? reg.try_get<Footprint>(wall) : nullptr;
    const Thing* thing = reg.valid(wall) ? reg.try_get<Thing>(wall) : nullptr;
    if (footprint == nullptr || thing == nullptr) {
        return;
    }
    const uint8_t full = obstacleFromFill(defs_.get(thing->def).number("fillPercent"));
    footprint->obstacleDm = reg.all_of<Window>(wall) ? numbers_.sillDm : full;
    if (reg.all_of<Position>(wall)) {
        const CellRect rect = footprint->rect();
        for (int y = rect.minY; y < rect.minY + rect.sizeY; ++y) {
            for (int x = rect.minX; x < rect.minX + rect.sizeX; ++x) {
                map_.setObstacle(CellPos{x, y, rect.level}, footprint->obstacleDm);
            }
        }
        map_.noteBuildingChange();
    }
}

}  // namespace colony
