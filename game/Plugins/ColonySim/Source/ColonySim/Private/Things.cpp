#include "ColonySim/Things.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Cover.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"

#include <algorithm>
#include <utility>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.
constexpr const char* kThingDefKind = "Things";

// Colonists jog at about 4 m/s (game design doc), in real seconds at 1x. Used only when a pawn
// def has no stats.moveSpeed.
constexpr float kThingFallbackJogSpeed = 4.0f;

bool thingCategoryFromDef(const Def& def, ThingCategory& out) {
    const std::string category = def.text("category");
    if (category == "Item") {
        out = ThingCategory::Item;
    } else if (category == "Building") {
        out = ThingCategory::Building;
    } else if (category == "Pawn") {
        out = ThingCategory::Pawn;
    } else {
        return false;
    }
    return true;
}

Position thingCellCentre(CellPos c) {
    return Position{static_cast<float>(c.x) + 0.5f, static_cast<float>(c.y) + 0.5f, c.level};
}

}  // namespace

Things::Things(Map& map, std::shared_ptr<const DefDatabase> defs) : map_(map), defs_(std::move(defs)) {}

Entity Things::spawn(DefId def, CellPos cell, int stackCount, Rot4 rot) {
    const Entity thing = make(def, stackCount);
    if (thing == kNoEntity) {
        return kNoEntity;
    }
    if (!spawnAt(thing, cell, rot)) {
        registry_.destroy(thing);
        return kNoEntity;
    }
    return thing;
}

Entity Things::spawn(const std::string& defName, CellPos cell, int stackCount, Rot4 rot) {
    return spawn(defs_->idOf(kThingDefKind, defName), cell, stackCount, rot);
}

Entity Things::make(DefId defId, int stackCount) {
    if (!defId.valid() || defId.value >= defs_->size()) {
        return kNoEntity;
    }
    const Def& def = defs_->get(defId);
    ThingCategory category = ThingCategory::Item;
    if (def.kind() != kThingDefKind || !thingCategoryFromDef(def, category)) {
        return kNoEntity;
    }

    const Entity thing = registry_.create();
    registry_.emplace<Thing>(thing, Thing{defId, category});
    if (category != ThingCategory::Pawn && def.has("stats.maxHitPoints")) {
        const int hp = static_cast<int>(def.integer("stats.maxHitPoints"));
        registry_.emplace<HitPoints>(thing, HitPoints{hp, hp});
    }
    switch (category) {
        case ThingCategory::Item: {
            const int limit = std::max(1, static_cast<int>(def.integer("stackLimit", 1)));
            registry_.emplace<StackCount>(thing, StackCount{std::clamp(stackCount, 1, limit)});
            break;
        }
        case ThingCategory::Building: {
            Footprint footprint;
            footprint.sizeX = std::max(1, static_cast<int>(def.numberAt("size", 0, 1.0)));
            footprint.sizeY = std::max(1, static_cast<int>(def.numberAt("size", 1, 1.0)));
            footprint.isDoor = def.text("thingClass") == "Building_Door";
            footprint.blocksMovement = !footprint.isDoor && def.text("passability") == "Impassable";
            footprint.obstacleDm = obstacleFromFill(def.number("fillPercent"));
            registry_.emplace<Footprint>(thing, footprint);
            break;
        }
        case ThingCategory::Pawn: {
            const double fallback = kThingFallbackJogSpeed / static_cast<double>(kStepsPerRealSecondAt1x);
            Mover mover;
            mover.metresPerStep = static_cast<float>(def.number("stats.moveSpeed", fallback));
            registry_.emplace<Mover>(thing, std::move(mover));
            if (def.has("race.body")) {
                registry_.emplace<Health>(thing);
            }
            break;
        }
    }
    return thing;
}

void Things::placeFootprint(Entity thing, const CellRect& rect, const Footprint& footprint) {
    const Passage passage = footprint.blocksMovement ? Passage::Blocked
                            : footprint.isDoor       ? Passage::Door
                                                     : Passage::Open;
    for (int y = rect.minY; y < rect.minY + rect.sizeY; ++y) {
        for (int x = rect.minX; x < rect.minX + rect.sizeX; ++x) {
            map_.setBuilding(CellPos{x, y, rect.level}, thing, passage, footprint.obstacleDm);
        }
    }
}

bool Things::spawnAt(Entity thing, CellPos cell, Rot4 rot) {
    if (!registry_.valid(thing) || registry_.all_of<Position>(thing)) {
        return false;
    }
    Footprint* footprint = registry_.try_get<Footprint>(thing);
    if (footprint == nullptr) {
        if (!map_.inBounds(cell) || !map_.groundWalkable(cell)) {
            return false;
        }
        registry_.emplace<Position>(thing, thingCellCentre(cell));
        report(EventType::ThingSpawned, thing, cell);
        return true;
    }

    const CellRect rect = footprintRect(cell, footprint->sizeX, footprint->sizeY, rot);
    for (int y = rect.minY; y < rect.minY + rect.sizeY; ++y) {
        for (int x = rect.minX; x < rect.minX + rect.sizeX; ++x) {
            const CellPos c{x, y, rect.level};
            if (!map_.inBounds(c) || !map_.groundWalkable(c) || map_.buildingAt(c) != kNoEntity) {
                return false;
            }
        }
    }
    placeFootprint(thing, rect, *footprint);
    footprint->origin = cell;
    footprint->rotation = rot;
    registry_.emplace<Position>(thing, Position{static_cast<float>(rect.minX) + 0.5f * static_cast<float>(rect.sizeX),
                                                static_cast<float>(rect.minY) + 0.5f * static_cast<float>(rect.sizeY),
                                                rect.level});
    report(EventType::ThingSpawned, thing, cell);
    return true;
}

bool Things::relinkBuilding(Entity thing) {
    if (!registry_.valid(thing) || !registry_.all_of<Position>(thing)) {
        return false;
    }
    const Footprint* footprint = registry_.try_get<Footprint>(thing);
    if (footprint == nullptr) {
        return true;
    }
    const CellRect rect = footprint->rect();
    for (int y = rect.minY; y < rect.minY + rect.sizeY; ++y) {
        for (int x = rect.minX; x < rect.minX + rect.sizeX; ++x) {
            const CellPos c{x, y, rect.level};
            if (!map_.inBounds(c) || !map_.groundWalkable(c) || map_.buildingAt(c) != kNoEntity) {
                return false;
            }
        }
    }
    placeFootprint(thing, rect, *footprint);
    return true;
}

void Things::despawn(Entity thing) {
    if (!spawned(thing)) {
        return;
    }
    if (const Footprint* footprint = registry_.try_get<Footprint>(thing)) {
        const CellRect rect = footprint->rect();
        for (int y = rect.minY; y < rect.minY + rect.sizeY; ++y) {
            for (int x = rect.minX; x < rect.minX + rect.sizeX; ++x) {
                map_.clearBuilding(CellPos{x, y, rect.level});
            }
        }
    }
    registry_.remove<Position>(thing);
    report(EventType::ThingDespawned, thing);
}

void Things::destroy(Entity thing) {
    if (!registry_.valid(thing)) {
        return;
    }
    despawn(thing);
    report(EventType::ThingDestroyed, thing);
    registry_.destroy(thing);
}

void Things::report(EventType type, Entity thing, CellPos cell) {
    if (events_ == nullptr) {
        return;
    }
    Event event;
    event.type = type;
    event.thing = thing;
    event.def = registry_.get<Thing>(thing).def;
    event.cell = cell;
    events_->push_back(std::move(event));
}

size_t Things::count() const {
    return registry_.view<const Thing>().size();
}

}  // namespace colony
