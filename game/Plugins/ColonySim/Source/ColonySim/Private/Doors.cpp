#include "ColonySim/Doors.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

DoorNumbers DoorNumbers::read(const DefDatabase& defs) {
    DoorNumbers n;
    const Def* def = defs.find("Rules", "DoorRules");
    if (def == nullptr) {
        return n;
    }
    // Ticks to steps: a tick is 1/60 of a real second, a step 1/30 (movement runs at real speed,
    // so this is not the 1.44 that work durations use).
    const double stepsPerTick = def->number("stepsPerTick", 0.5);
    n.openSteps = std::max(1, static_cast<int>(std::ceil(def->number("openTicks", 45.0) * stepsPerTick)));
    n.closeDelaySteps = std::max(1, static_cast<int>(std::ceil(def->number("closeDelayTicks", 110.0) * stepsPerTick)));
    n.pathCost = static_cast<float>(def->number("pathCost", n.pathCost));
    return n;
}

void setDoorSight(Map& map, const Footprint& footprint, bool open) {
    const CellRect rect = footprint.rect();
    for (int y = rect.minY; y < rect.minY + rect.sizeY; ++y) {
        for (int x = rect.minX; x < rect.minX + rect.sizeX; ++x) {
            map.setObstacle(CellPos{x, y, rect.level}, open ? uint8_t{0} : footprint.obstacleDm);
        }
    }
}

}  // namespace colony
