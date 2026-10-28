// Nutrient paste: a dispenser turns the raw food in the hoppers beside it into meals (docs/design/nutrient-paste.md).
// Part of the job system: a hungry colonist picks the dispenser like any food (NeedJobs.cpp `findFood`).
#include "ColonySim/Jobs.h"

#include "ColonySim/Construction.h"
#include "ColonySim/Map.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace colony {

namespace {

// The cells touching the footprint's sides (not its corners), in row order: below it, left, right, above it.
std::vector<CellPos> pasteSideCells(const CellRect& rect) {
    std::vector<CellPos> cells;
    for (int x = rect.minX; x < rect.minX + rect.sizeX; ++x) {
        cells.push_back(CellPos{x, rect.minY - 1, rect.level});
    }
    for (int y = rect.minY; y < rect.minY + rect.sizeY; ++y) {
        cells.push_back(CellPos{rect.minX - 1, y, rect.level});
        cells.push_back(CellPos{rect.minX + rect.sizeX, y, rect.level});
    }
    for (int x = rect.minX; x < rect.minX + rect.sizeX; ++x) {
        cells.push_back(CellPos{x, rect.minY + rect.sizeY, rect.level});
    }
    return cells;
}

}  // namespace

float JobSystem::pasteStock(Entity dispenser) const {
    const entt::registry& reg = things_.registry();
    if (!things_.spawned(dispenser) || reg.all_of<ConstructionSite>(dispenser) || !reg.all_of<Footprint>(dispenser)) {
        return 0.0f;
    }
    float stock = 0.0f;
    for (const CellPos& cell : pasteSideCells(reg.get<Footprint>(dispenser).rect())) {
        const Entity hopper = map_.inBounds(cell) ? map_.buildingAt(cell) : kNoEntity;
        if (hopper == kNoEntity || isHopper_[reg.get<Thing>(hopper).def.value] == 0 || reg.all_of<ConstructionSite>(hopper)) {
            continue;
        }
        for (const Entity item : things_.itemsAt(cell)) {
            const DefId def = reg.get<Thing>(item).def;
            stock += pasteRaw_[def.value] != 0 ? nutrition_[def.value] * static_cast<float>(reg.get<StackCount>(item).count) : 0.0f;
        }
    }
    return stock;
}

bool JobSystem::pasteSpot(Entity dispenser, Entity pawn, CellPos& cell) const {
    const entt::registry& reg = things_.registry();
    const Position& at = reg.get<Position>(pawn);
    const CellPos from = cellOf(at);
    float bestDist = 0.0f;
    bool found = false;
    for (const CellPos& c : pasteSideCells(reg.get<Footprint>(dispenser).rect())) {
        if (!map_.walkable(c) || map_.buildingAt(c) != kNoEntity || !things_.itemsAt(c).empty() ||
            reservedByOther(JobTarget{kNoEntity, c}, pawn) || avoiding(pawn, JobTarget{kNoEntity, c}) || !canReach(from, c)) {
            continue;
        }
        const float dx = static_cast<float>(c.x) + 0.5f - at.x;
        const float dy = static_cast<float>(c.y) + 0.5f - at.y;
        const float dist = dx * dx + dy * dy;
        if (!found || dist < bestDist) {
            found = true;
            bestDist = dist;
            cell = c;
        }
    }
    return found;
}

Entity JobSystem::dispensePaste(Entity dispenser, CellPos cell) {
    entt::registry& reg = things_.registry();
    const DefId dispenserDef = reg.get<Thing>(dispenser).def;
    float needed = pasteCost_[dispenserDef.value];
    // Hoppers in row order, each stack as far as it is needed: whole items, so a little may go to waste (RimWorld's).
    for (const CellPos& side : pasteSideCells(reg.get<Footprint>(dispenser).rect())) {
        const Entity hopper = map_.inBounds(side) ? map_.buildingAt(side) : kNoEntity;
        if (needed <= 0.0f || hopper == kNoEntity || isHopper_[reg.get<Thing>(hopper).def.value] == 0 ||
            reg.all_of<ConstructionSite>(hopper)) {
            continue;
        }
        const std::vector<Entity> items = things_.itemsAt(side);
        for (const Entity item : items) {
            const DefId def = reg.get<Thing>(item).def;
            if (needed <= 0.0f || pasteRaw_[def.value] == 0) {
                continue;
            }
            const int have = reg.get<StackCount>(item).count;
            const int take = std::min(have, static_cast<int>(std::ceil(needed / nutrition_[def.value] - 0.0001f)));
            needed -= nutrition_[def.value] * static_cast<float>(take);
            if (take >= have) {
                things_.destroy(item);
            } else {
                reg.get<StackCount>(item).count -= take;
            }
        }
    }
    return things_.spawn(pasteMeal_[dispenserDef.value], cell, 1);
}

}  // namespace colony
