#include "ColonySim/RoofCollapse.h"

#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Things.h"

#include <algorithm>

namespace colony {

namespace {

float roofNum(const Def& def, const char* path, float fallback) {
    return static_cast<float>(def.number(path, fallback));
}

int roofInt(const Def& def, const char* path, int fallback) {
    return static_cast<int>(def.integer(path, fallback));
}

}  // namespace

RoofNumbers RoofNumbers::read(const DefDatabase& defs) {
    RoofNumbers n;
    if (const Def* def = defs.find("Rules", "RoofRules")) {
        n.thinPawnHits = roofInt(*def, "thinPawnHits", n.thinPawnHits);
        n.thinPawnDamage = roofNum(*def, "thinPawnDamage", n.thinPawnDamage);
        n.thickPawnHits = roofInt(*def, "thickPawnHits", n.thickPawnHits);
        n.thickPawnDamage = roofNum(*def, "thickPawnDamage", n.thickPawnDamage);
        n.thinThingDamage = roofInt(*def, "thinThingDamage", n.thinThingDamage);
        n.thickThingDamage = roofInt(*def, "thickThingDamage", n.thickThingDamage);
        n.fallPawnHits = roofInt(*def, "fallPawnHits", n.fallPawnHits);
        n.fallPawnDamage = roofNum(*def, "fallPawnDamage", n.fallPawnDamage);
        n.fallSearchRadius = std::max(0, roofInt(*def, "fallSearchRadius", n.fallSearchRadius));
    }
    return n;
}

RoofCollapse::RoofCollapse(Map& map, Rooms& rooms, Things& things, HealthSystem& health, const DefDatabase& defs)
    : map_(map), rooms_(rooms), things_(things), health_(health), numbers_(RoofNumbers::read(defs)) {
    crushType_ = defs.idOf("DamageTypes", "Crush");
    const int reach = numbers_.fallSearchRadius;
    for (int dy = -reach; dy <= reach; ++dy) {
        for (int dx = -reach; dx <= reach; ++dx) {
            landingOffsets_.emplace_back(dx, dy);
        }
    }
    std::sort(landingOffsets_.begin(), landingOffsets_.end(), [](const auto& a, const auto& b) {
        const int da = a.first * a.first + a.second * a.second;
        const int db = b.first * b.first + b.second * b.second;
        return da != db ? da < db : (a.second != b.second ? a.second < b.second : a.first < b.first);
    });
}

void RoofCollapse::step(Rng& rng) {
    // A fall can take a wall, and with it the holder of the next roof: go on until nothing moves.
    for (int pass = 0; pass < 16; ++pass) {
        rooms_.update();
        bool moved = dropThinRoofs(rng);
        moved = dropThickRoofs(rng) || moved;
        moved = dropFloors(rng) || moved;
        if (!moved) {
            break;
        }
    }
}

bool RoofCollapse::dropThinRoofs(Rng& rng) {
    const std::vector<CellPos> cells = rooms_.takeUnsupportedRoofs();
    for (const CellPos& c : cells) {
        crush(c, Kind::Thin, rng);
    }
    return !cells.empty();
}

bool RoofCollapse::dropThickRoofs(Rng& rng) {
    const uint64_t buildings = map_.buildingChanges();
    const uint64_t rock = map_.rockChanges();
    if (buildings == thickBuildings_ && rock == thickRock_) {
        return false;
    }
    thickBuildings_ = buildings;
    thickRock_ = rock;
    std::vector<int> unsupported;
    for (const int i : map_.hollowRoofCells()) {
        const CellPos c = map_.cellAt(i);
        if (!rooms_.roofSupported(c)) {
            unsupported.push_back(i);
        }
    }
    std::sort(unsupported.begin(), unsupported.end());
    for (const int i : unsupported) {
        map_.setThickRoof(map_.cellAt(i), false);
        fallenThick_.push_back(i);
        crush(map_.cellAt(i), Kind::Thick, rng);
    }
    return !unsupported.empty();
}

bool RoofCollapse::dropFloors(Rng& rng) {
    const uint64_t buildings = map_.buildingChanges();
    const uint64_t changes = map_.upperFloorChanges();
    if (buildings == floorBuildings_ && changes == floorChanges_) {
        return false;
    }
    floorBuildings_ = buildings;
    floorChanges_ = changes;
    bool any = false;
    // Lowest level first: a floor that falls takes its walls, and so the floors they held up.
    for (const CellPos& c : map_.upperFloorCells()) {
        if (map_.upperFloor(c) && !rooms_.roofSupported(CellPos{c.x, c.y, c.level - 1})) {
            dropFloor(c, rng);
            any = true;
        }
    }
    return any;
}

void RoofCollapse::crush(CellPos c, Kind kind, Rng& rng) {
    entt::registry& reg = things_.registry();
    const bool thick = kind == Kind::Thick;
    const int thingDamage = thick ? numbers_.thickThingDamage : numbers_.thinThingDamage;
    const auto hurtThing = [&](Entity thing) {
        if (!reg.valid(thing)) {
            return;
        }
        HitPoints* hp = reg.try_get<HitPoints>(thing);
        if (hp == nullptr) {
            return;
        }
        hp->current -= thingDamage;
        if (hp->current <= 0) {
            things_.destroy(thing);
        }
    };
    int hurt = 0;
    const Entity building = map_.buildingAt(c);
    if (building != kNoEntity) {
        hurtThing(building);
        ++hurt;
    }
    const std::vector<Entity> items = things_.itemsAt(c);
    for (const Entity item : items) {
        hurtThing(item);
        ++hurt;
    }
    std::vector<Entity> pawns;
    for (const Entity pawn : reg.view<const Position, const Mover>()) {
        if (cellOf(reg.get<Position>(pawn)) == c) {
            pawns.push_back(pawn);
        }
    }
    std::sort(pawns.begin(), pawns.end());
    const int hits = thick ? numbers_.thickPawnHits : numbers_.thinPawnHits;
    const float damage = thick ? numbers_.thickPawnDamage : numbers_.thinPawnDamage;
    for (const Entity pawn : pawns) {
        bool struck = false;
        for (int i = 0; i < hits; ++i) {
            struck = health_.applyDamage(pawn, crushType_, damage, -1, rng) || struck;
        }
        if (struck) {
            ++hurt;
            if (interrupt_) {
                interrupt_(pawn, "the roof fell on it");
            }
        }
    }
    report(c, kind, hurt);
}

bool RoofCollapse::landingCell(CellPos around, CellPos* out) const {
    for (const auto& [dx, dy] : landingOffsets_) {
        const CellPos n{around.x + dx, around.y + dy, around.level};
        if (map_.walkable(n)) {
            *out = n;
            return true;
        }
    }
    return false;
}

void RoofCollapse::dropFloor(CellPos c, Rng& rng) {
    entt::registry& reg = things_.registry();
    const CellPos below{c.x, c.y, c.level - 1};
    int hurt = 0;

    // Who stands on it falls to the level below, and is hurt by the fall.
    std::vector<Entity> pawns;
    for (const Entity pawn : reg.view<const Position, const Mover>()) {
        if (cellOf(reg.get<Position>(pawn)) == c) {
            pawns.push_back(pawn);
        }
    }
    std::sort(pawns.begin(), pawns.end());
    for (const Entity pawn : pawns) {
        CellPos land;
        if (landingCell(below, &land)) {
            Position& position = reg.get<Position>(pawn);
            position.x = static_cast<float>(land.x) + 0.5f;
            position.y = static_cast<float>(land.y) + 0.5f;
            position.level = land.level;
            Mover& mover = reg.get<Mover>(pawn);
            mover.path.clear();
            mover.next = 0;
            mover.vx = mover.vy = 0.0f;
        }
        bool struck = false;
        for (int i = 0; i < numbers_.fallPawnHits; ++i) {
            struck = health_.applyDamage(pawn, crushType_, numbers_.fallPawnDamage, -1, rng) || struck;
        }
        hurt += struck ? 1 : 0;
        if (interrupt_) {
            interrupt_(pawn, "the floor fell under it");
        }
    }
    // Things lying on it fall too.
    const std::vector<Entity> items = things_.itemsAt(c);
    for (const Entity item : items) {
        CellPos land;
        things_.despawn(item);
        if (!landingCell(below, &land) || !things_.spawnAt(item, land)) {
            things_.destroy(item);
        }
        ++hurt;
    }
    // A building on a floor that falls is lost.
    const Entity building = map_.buildingAt(c);
    if (building != kNoEntity) {
        things_.destroy(building);
        ++hurt;
    }
    map_.setUpperFloor(c, false);
    report(c, Kind::Floor, hurt);
}

void RoofCollapse::report(CellPos c, Kind kind, int hurt) {
    ++collapses_;
    if (events_ == nullptr) {
        return;
    }
    Event event;
    event.type = EventType::RoofCollapsed;
    event.cell = c;
    event.message = kind == Kind::Thin ? "roof" : (kind == Kind::Thick ? "mountain" : "floor");
    event.count = hurt;
    events_->push_back(std::move(event));
}

RoofCollapseSaved RoofCollapse::state() const {
    return RoofCollapseSaved{rooms_.thinRoofCells(), fallenThick_, rooms_.unsupportedRoofs(), collapses_};
}

bool RoofCollapse::restore(const RoofCollapseSaved& saved) {
    for (const int i : saved.fallenThick) {
        if (i < 0 || i >= map_.width() * map_.height()) {
            return false;
        }
    }
    for (const CellPos& c : saved.pending) {
        if (!map_.inBounds(c)) {
            return false;
        }
    }
    fallenThick_ = saved.fallenThick;
    for (const int i : fallenThick_) {
        map_.setThickRoof(map_.cellAt(i), false);
    }
    rooms_.restoreUnsupportedRoofs(saved.pending);
    collapses_ = saved.collapses;
    thickBuildings_ = thickRock_ = floorBuildings_ = floorChanges_ = ~uint64_t{0};
    return true;
}

void RoofCollapse::hashState(StateHasher& hasher) const {
    hasher.add(collapses_);
    hasher.add(static_cast<uint64_t>(fallenThick_.size()));
    for (const int i : fallenThick_) {
        hasher.add(i);
    }
    hasher.add(static_cast<uint64_t>(rooms_.unsupportedRoofs().size()));
}

}  // namespace colony
