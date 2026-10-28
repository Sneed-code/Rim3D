#include "ColonySim/Things.h"

#include "ColonySim/Animals.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Cover.h"
#include "ColonySim/Artillery.h"
#include "ColonySim/Defense.h"
#include "ColonySim/Doors.h"
#include "ColonySim/Fuel.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Power.h"

#include <algorithm>
#include <iterator>
#include <string>
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

uint64_t thingCellKey(CellPos c) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(c.level)) << 48) |
           (static_cast<uint64_t>(static_cast<uint32_t>(c.y) & 0xFFFFFFu) << 24) |
           static_cast<uint64_t>(static_cast<uint32_t>(c.x) & 0xFFFFFFu);
}

const std::vector<Entity>& thingNoItems() {
    static const std::vector<Entity> none;
    return none;
}

}  // namespace

std::vector<LevelLink> levelLinksOf(const CellRect& rect, Rot4 rot, Entity building, float speedFactor) {
    std::vector<LevelLink> out;
    if (rect.level + 1 >= Map::kLevels) {
        return out;
    }
    const bool alongY = rot == Rot4::North || rot == Rot4::South;
    const int width = alongY ? rect.sizeX : rect.sizeY;
    const int run = alongY ? rect.sizeY : rect.sizeX;
    const float flat = static_cast<float>(run + 1);
    const float length = std::sqrt(flat * flat + Map::kLevelHeightM * Map::kLevelHeightM);
    for (int k = 0; k < width; ++k) {
        LevelLink link;
        link.building = building;
        link.length = length;
        link.speedFactor = speedFactor;
        const int maxX = rect.minX + rect.sizeX - 1;
        const int maxY = rect.minY + rect.sizeY - 1;
        switch (rot) {
            case Rot4::North:
                link.low = CellPos{rect.minX + k, rect.minY - 1, rect.level};
                link.high = CellPos{rect.minX + k, maxY + 1, rect.level + 1};
                break;
            case Rot4::South:
                link.low = CellPos{rect.minX + k, maxY + 1, rect.level};
                link.high = CellPos{rect.minX + k, rect.minY - 1, rect.level + 1};
                break;
            case Rot4::East:
                link.low = CellPos{rect.minX - 1, rect.minY + k, rect.level};
                link.high = CellPos{maxX + 1, rect.minY + k, rect.level + 1};
                break;
            case Rot4::West:
                link.low = CellPos{maxX + 1, rect.minY + k, rect.level};
                link.high = CellPos{rect.minX - 1, rect.minY + k, rect.level + 1};
                break;
        }
        out.push_back(link);
    }
    return out;
}

void Things::linkLevels(Entity thing) {
    const Footprint* footprint = registry_.try_get<Footprint>(thing);
    const Def& def = defs_->get(registry_.get<Thing>(thing).def);
    if (footprint == nullptr || !def.has("building.climb")) {
        return;
    }
    const float speed = static_cast<float>(def.number("building.climb.speedFactor", 1.0));
    for (const LevelLink& link : levelLinksOf(footprint->rect(), footprint->rotation, thing, speed)) {
        map_.addLevelLink(link);
    }
}

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
            if (thingHasSpoilage(def)) {
                registry_.emplace<Spoilage>(thing);
            }
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
            if (footprint.isDoor) {
                registry_.emplace<Door>(thing);  // closed (Doors.h)
            }
            if (def.flag("building.bed")) {
                registry_.emplace<Bed>(thing);  // who sleeps in it (Needs.h)
            }
            if (def.has("power")) {
                registry_.emplace<PowerPart>(thing);  // switch, output, charge (Power.h)
            }
            if (def.has("turret")) {
                registry_.emplace<Turret>(thing);  // Defense.h
            }
            if (def.has("artillery")) {
                registry_.emplace<Artillery>(thing);  // Artillery.h
            }
            if (def.has("trap")) {
                registry_.emplace<Trap>(thing);
            }
            if (const FuelSpec fuelSpec = readFuelSpec(*defs_, def); fuelSpec.present) {
                registry_.emplace<Fuel>(thing, Fuel{fuelSpec.capacity * fuelSpec.initial});  // Fuel.h
            }
            break;
        }
        case ThingCategory::Pawn: {
            const double fallback = kThingFallbackJogSpeed / static_cast<double>(kStepsPerRealSecondAt1x);
            Mover mover;
            mover.metresPerStep = static_cast<float>(def.number("stats.moveSpeed", fallback));
            registry_.emplace<Mover>(thing, std::move(mover));
            registry_.emplace<Posture>(thing);
            if (def.has("race.body")) {
                registry_.emplace<Health>(thing);
            }
            if (def.text("race.intelligence") == "Animal") {
                registry_.emplace<Animal>(thing, makeAnimalFor(def));  // Animals.h
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

bool Things::standsOnFlowingWater(Entity thing) const {
    const Thing* t = registry_.try_get<Thing>(thing);
    return t != nullptr && t->def.valid() && defs_->get(t->def).flag("building.onFlowingWater");
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
        if (registry_.all_of<StackCount>(thing)) {
            indexItem(thing, cell, true);
        }
        report(EventType::ThingSpawned, thing, cell);
        return true;
    }

    const CellRect rect = footprintRect(cell, footprint->sizeX, footprint->sizeY, rot);
    const bool onWater = standsOnFlowingWater(thing);
    for (int y = rect.minY; y < rect.minY + rect.sizeY; ++y) {
        for (int x = rect.minX; x < rect.minX + rect.sizeX; ++x) {
            const CellPos c{x, y, rect.level};
            if (!map_.inBounds(c) || !map_.groundBuildable(c, onWater) || map_.buildingAt(c) != kNoEntity) {
                return false;
            }
        }
    }
    placeFootprint(thing, rect, *footprint);
    footprint->origin = cell;
    footprint->rotation = rot;
    linkLevels(thing);
    registry_.emplace<Position>(thing, Position{static_cast<float>(rect.minX) + 0.5f * static_cast<float>(rect.sizeX),
                                                static_cast<float>(rect.minY) + 0.5f * static_cast<float>(rect.sizeY),
                                                rect.level});
    report(EventType::ThingSpawned, thing, cell);
    return true;
}

bool Things::relink(Entity thing) {
    if (!registry_.valid(thing) || !registry_.all_of<Position>(thing)) {
        return false;
    }
    const Footprint* footprint = registry_.try_get<Footprint>(thing);
    if (footprint == nullptr) {
        if (registry_.all_of<StackCount>(thing)) {
            indexItem(thing, cellOf(registry_.get<Position>(thing)), true);
        }
        return true;
    }
    const CellRect rect = footprint->rect();
    const bool onWater = standsOnFlowingWater(thing);
    for (int y = rect.minY; y < rect.minY + rect.sizeY; ++y) {
        for (int x = rect.minX; x < rect.minX + rect.sizeX; ++x) {
            const CellPos c{x, y, rect.level};
            if (!map_.inBounds(c) || !map_.groundBuildable(c, onWater) || map_.buildingAt(c) != kNoEntity) {
                return false;
            }
        }
    }
    placeFootprint(thing, rect, *footprint);
    linkLevels(thing);
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
        map_.removeLevelLinks(thing);
    }
    if (registry_.all_of<StackCount>(thing)) {
        indexItem(thing, cellOf(registry_.get<Position>(thing)), false);
    }
    const CellPos was = cellOf(registry_.get<Position>(thing));
    registry_.remove<Position>(thing);
    report(EventType::ThingDespawned, thing, was);  // the cell it left (a building's origin cell, an item's own)
}

void Things::indexItem(Entity thing, CellPos cell, bool add) {
    ++itemChanges_;
    const uint64_t key = thingCellKey(cell);
    if (add) {
        std::vector<Entity>& list = itemsAt_[key];
        list.insert(std::lower_bound(list.begin(), list.end(), thing), thing);
        return;
    }
    const auto it = itemsAt_.find(key);
    if (it == itemsAt_.end()) {
        return;
    }
    std::vector<Entity>& list = it->second;
    list.erase(std::remove(list.begin(), list.end(), thing), list.end());
    if (list.empty()) {
        itemsAt_.erase(it);
    }
}

const std::vector<Entity>& Things::itemsAt(CellPos cell) const {
    const auto it = itemsAt_.find(thingCellKey(cell));
    return it == itemsAt_.end() ? thingNoItems() : it->second;
}

Entity Things::splitOff(Entity item, int count) {
    StackCount* stack = registry_.valid(item) ? registry_.try_get<StackCount>(item) : nullptr;
    if (stack == nullptr || count <= 0 || count >= stack->count) {
        return kNoEntity;
    }
    const Entity part = make(registry_.get<Thing>(item).def, count);
    if (part == kNoEntity) {
        return kNoEntity;
    }
    registry_.get<StackCount>(item).count -= count;  // `stack` may have moved: make() added a StackCount
    if (const HitPoints* hp = registry_.try_get<HitPoints>(item)) {
        registry_.emplace_or_replace<HitPoints>(part, *hp);
    }
    if (const Spoilage* spoilage = registry_.try_get<Spoilage>(item)) {
        registry_.emplace_or_replace<Spoilage>(part, *spoilage);
    }
    return part;
}

void Things::addToStack(Entity into, Entity from, int count) {
    StackCount& stack = registry_.get<StackCount>(into);
    Spoilage* a = registry_.try_get<Spoilage>(into);
    const Spoilage* b = registry_.try_get<Spoilage>(from);
    if (a != nullptr && b != nullptr && count > 0) {
        const float have = static_cast<float>(stack.count);
        const float put = static_cast<float>(count);
        a->rot = (a->rot * have + b->rot * put) / (have + put);
        a->decay = std::max(a->decay, b->decay);
        a->poisoned = a->poisoned || b->poisoned;
        std::vector<DefId> joined;
        std::set_union(a->ingredients.begin(), a->ingredients.end(), b->ingredients.begin(), b->ingredients.end(),
                       std::back_inserter(joined), [](DefId x, DefId y) { return x.value < y.value; });
        a->ingredients = std::move(joined);
    }
    stack.count += count;
}

bool thingHasSpoilage(const Def& def) {
    return def.text("category") == "Item" &&
           (thingRotDays(def) > 0.0 || def.number("stats.deteriorationRate") > 0.0 || def.text("ingestible.foodType") == "Meal");
}

double thingRotDays(const Def& def) {
    for (size_t i = 0, n = def.count("comps"); i < n; ++i) {
        const std::string path = "comps." + std::to_string(i);
        if (def.text((path + ".type").c_str()) == "Rottable") {
            return def.number((path + ".daysToRotStart").c_str());
        }
    }
    return 0.0;
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
