#include "ColonySim/Construction.h"

#include "ColonySim/Colonist.h"
#include "ColonySim/Genes.h"
#include "ColonySim/Map.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Random.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

// How far from a building (in cells, rings around its middle) things pushed out of the way or
// given back are looked for room.
constexpr int kConstructionDropRadius = 12;

constexpr const char* kConstructionSkill = "Construction";

uint64_t constructionCellKey(CellPos c) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(c.level)) << 48) |
           (static_cast<uint64_t>(static_cast<uint32_t>(c.y) & 0xFFFFFFu) << 24) |
           static_cast<uint64_t>(static_cast<uint32_t>(c.x) & 0xFFFFFFu);
}

std::string constructionCellText(CellPos c) {
    return "(" + std::to_string(c.x) + ", " + std::to_string(c.y) + ")";
}

Position constructionRectCentre(const CellRect& rect) {
    return Position{static_cast<float>(rect.minX) + 0.5f * static_cast<float>(rect.sizeX),
                    static_cast<float>(rect.minY) + 0.5f * static_cast<float>(rect.sizeY), rect.level};
}

// The cells of the square ring at Chebyshev distance `r` around (cx, cy), in row order; r = 0 is
// the cell itself.
void constructionRing(int cx, int cy, int r, int level, std::vector<CellPos>& out) {
    out.clear();
    for (int y = cy - r; y <= cy + r; ++y) {
        for (int x = cx - r; x <= cx + r; ++x) {
            if (std::abs(x - cx) == r || std::abs(y - cy) == r) {
                out.push_back(CellPos{x, y, level});
            }
        }
    }
}

}  // namespace

bool ConstructionSite::hasAllMaterials() const {
    return std::all_of(materials.begin(), materials.end(), [](const MaterialCount& m) { return m.have >= m.needed; });
}

int ConstructionSite::materialsNeeded() const {
    int total = 0;
    for (const MaterialCount& m : materials) {
        total += m.needed;
    }
    return total;
}

int ConstructionSite::materialsHave() const {
    int total = 0;
    for (const MaterialCount& m : materials) {
        total += std::min(m.have, m.needed);
    }
    return total;
}

Construction::Construction(Map& map, Things& things, Plants& plants, const DefDatabase& defs, Rng& rng,
                           std::vector<Event>& events)
    : map_(map), things_(things), plants_(plants), defs_(defs), rng_(rng), events_(events) {
    if (const Def* rules = defs_.find("Rules", "ConstructionRules")) {
        numbers_.buildWorkPerSecond = rules->number("buildWorkPerSecond", numbers_.buildWorkPerSecond);
        numbers_.deconstructWorkPerSecond = rules->number("deconstructWorkPerSecond", numbers_.deconstructWorkPerSecond);
        numbers_.deconstructWorkMin = rules->number("deconstructWorkMin", numbers_.deconstructWorkMin);
        numbers_.deconstructWorkMax = rules->number("deconstructWorkMax", numbers_.deconstructWorkMax);
        numbers_.deconstructRefundFactor = rules->number("deconstructRefundFactor", numbers_.deconstructRefundFactor);
        numbers_.skillSpeedBase = rules->number("skillSpeedBase", numbers_.skillSpeedBase);
        numbers_.skillSpeedPerLevel = rules->number("skillSpeedPerLevel", numbers_.skillSpeedPerLevel);
        numbers_.defaultSkillLevel = static_cast<int>(rules->integer("defaultSkillLevel", numbers_.defaultSkillLevel));
    }
}

// ---- Costs ----------------------------------------------------------------------------------

bool Construction::buildable(DefId def) const {
    if (!def.valid() || def.value >= defs_.size()) {
        return false;
    }
    const Def& d = defs_.get(def);
    return d.kind() == "Things" && d.text("category") == "Building" && d.has("stats.workToBuild");
}

bool Construction::stuffFits(DefId def, DefId stuff) const {
    const Def& d = defs_.get(def);
    if (d.count("stuffCategories") == 0) {
        return !stuff.valid();
    }
    if (!stuff.valid() || stuff.value >= defs_.size()) {
        return false;
    }
    const Def& s = defs_.get(stuff);
    if (s.kind() != "Things") {
        return false;
    }
    for (size_t i = 0; i < d.count("stuffCategories"); ++i) {
        const std::string wanted = d.textAt("stuffCategories", i);
        for (size_t j = 0; j < s.count("stuffProps.categories"); ++j) {
            if (s.textAt("stuffProps.categories", j) == wanted) {
                return true;
            }
        }
    }
    return false;
}

std::vector<Construction::Cost> Construction::costOf(DefId def, DefId stuff) const {
    std::vector<Cost> costs;
    const auto add = [&costs](DefId item, int count) {
        if (!item.valid() || count <= 0) {
            return;
        }
        for (Cost& c : costs) {
            if (c.item == item) {
                c.count += count;
                return;
            }
        }
        costs.push_back(Cost{item, count});
    };
    const Def& d = defs_.get(def);
    for (const std::string& key : d.keys("costs")) {
        add(defs_.idOf("Things", key), static_cast<int>(d.integer(("costs." + key).c_str())));
    }
    if (stuff.valid()) {
        add(stuff, static_cast<int>(d.integer("costStuffCount")));
    }
    return costs;
}

float Construction::workToBuild(DefId def, DefId stuff) const {
    const Def& d = defs_.get(def);
    double work = d.number("stats.workToBuild");
    if (stuff.valid()) {
        work *= defs_.get(stuff).number("stuffProps.statFactors.workToBuild", 1.0);
    }
    return static_cast<float>(work);
}

int Construction::maxHitPoints(DefId def, DefId stuff) const {
    const Def& d = defs_.get(def);
    if (!d.has("stats.maxHitPoints")) {
        return 0;
    }
    double hp = d.number("stats.maxHitPoints");
    if (stuff.valid()) {
        hp *= defs_.get(stuff).number("stuffProps.statFactors.maxHitPoints", 1.0);
    }
    return std::max(1, static_cast<int>(std::lround(hp)));
}

int Construction::stackLimit(DefId def) const {
    return std::max(1, static_cast<int>(defs_.get(def).integer("stackLimit", 1)));
}

// ---- Sites ----------------------------------------------------------------------------------

Entity Construction::siteAt(CellPos cell) const {
    const auto it = siteAt_.find(constructionCellKey(cell));
    if (it == siteAt_.end()) {
        return kNoEntity;
    }
    // A site destroyed some other way (the dev command) leaves its cells behind in the index.
    const entt::registry& reg = things_.registry();
    return reg.valid(it->second) && reg.all_of<ConstructionSite>(it->second) ? it->second : kNoEntity;
}

void Construction::indexSite(Entity site, bool add) {
    const CellRect rect = things_.registry().get<ConstructionSite>(site).rect();
    for (int y = rect.minY; y < rect.minY + rect.sizeY; ++y) {
        for (int x = rect.minX; x < rect.minX + rect.sizeX; ++x) {
            const uint64_t key = constructionCellKey(CellPos{x, y, rect.level});
            if (add) {
                siteAt_[key] = site;
            } else {
                const auto it = siteAt_.find(key);
                if (it != siteAt_.end() && it->second == site) {
                    siteAt_.erase(it);
                }
            }
        }
    }
}

Entity Construction::placeBlueprint(DefId def, DefId stuff, CellPos cell, Rot4 rot, std::string& error) {
    if (!buildable(def)) {
        error = "that can't be built";
        return kNoEntity;
    }
    const Def& d = defs_.get(def);
    if (!stuffFits(def, stuff)) {
        error = d.count("stuffCategories") == 0 ? "that is not made of anything" : "it can't be made of that";
        return kNoEntity;
    }
    const int sizeX = std::max(1, static_cast<int>(d.numberAt("size", 0, 1.0)));
    const int sizeY = std::max(1, static_cast<int>(d.numberAt("size", 1, 1.0)));
    const CellRect rect = footprintRect(cell, sizeX, sizeY, rot);
    for (int y = rect.minY; y < rect.minY + rect.sizeY; ++y) {
        for (int x = rect.minX; x < rect.minX + rect.sizeX; ++x) {
            const CellPos c{x, y, rect.level};
            if (!map_.inBounds(c) || !map_.groundWalkable(c)) {
                error = "can't build on " + constructionCellText(c);
                return kNoEntity;
            }
            if (map_.buildingAt(c) != kNoEntity) {
                error = "there is a building at " + constructionCellText(c);
                return kNoEntity;
            }
            if (siteAt(c) != kNoEntity) {
                error = "something is already planned at " + constructionCellText(c);
                return kNoEntity;
            }
        }
    }

    ConstructionSite site;
    site.stuff = stuff;
    site.origin = cell;
    site.rotation = rot;
    site.sizeX = sizeX;
    site.sizeY = sizeY;
    for (const Cost& cost : costOf(def, stuff)) {
        site.materials.push_back(MaterialCount{cost.item, cost.count, 0});
    }
    site.workTotal = workToBuild(def, stuff);

    entt::registry& reg = things_.registry();
    const Entity e = reg.create();
    reg.emplace<Thing>(e, Thing{def, ThingCategory::Building});
    reg.emplace<Position>(e, constructionRectCentre(rect));
    reg.emplace<ConstructionSite>(e, std::move(site));
    indexSite(e, true);

    Event event;
    event.type = EventType::ThingSpawned;
    event.thing = e;
    event.def = def;
    event.cell = cell;
    events_.push_back(std::move(event));
    return e;
}

bool Construction::cancel(Entity thing, std::string& error) {
    entt::registry& reg = things_.registry();
    if (!reg.valid(thing) || !reg.all_of<Thing>(thing)) {
        error = "that doesn't exist";
        return false;
    }
    if (const ConstructionSite* site = reg.try_get<ConstructionSite>(thing)) {
        const ConstructionSite copy = *site;
        indexSite(thing, false);
        things_.destroy(thing);
        for (const MaterialCount& m : copy.materials) {
            dropNear(m.item, m.have, copy.rect());
        }
        return true;
    }
    if (reg.all_of<MarkedForDeconstruct>(thing)) {
        reg.remove<MarkedForDeconstruct>(thing);
        return true;
    }
    error = "there is nothing planned there to cancel";
    return false;
}

bool Construction::markDeconstruct(Entity building, std::string& error) {
    entt::registry& reg = things_.registry();
    if (!things_.spawned(building) || !reg.all_of<Thing, Footprint>(building) || reg.all_of<ConstructionSite>(building)) {
        error = "that isn't a building";
        return false;
    }
    if (reg.all_of<MarkedForDeconstruct>(building)) {
        error = "it is already marked";
        return false;
    }
    reg.emplace<MarkedForDeconstruct>(building);
    return true;
}

int Construction::deliver(Entity site, DefId item, int count) {
    entt::registry& reg = things_.registry();
    ConstructionSite* s = reg.valid(site) ? reg.try_get<ConstructionSite>(site) : nullptr;
    if (s == nullptr || count <= 0) {
        return 0;
    }
    for (MaterialCount& m : s->materials) {
        if (m.item == item && m.have < m.needed) {
            const int taken = std::min(count, m.needed - m.have);
            m.have += taken;
            if (s->stage == ConstructionStage::Blueprint) {
                s->stage = ConstructionStage::Frame;
                Event event;
                event.type = EventType::FrameStarted;
                event.thing = site;
                event.def = reg.get<Thing>(site).def;
                event.cell = s->origin;
                events_.push_back(std::move(event));
            }
            return taken;
        }
    }
    return 0;
}

// ---- Finishing and taking down ----------------------------------------------------------------

void Construction::clearFootprint(Entity, const CellRect& rect, bool blocksMovement, bool standable) {
    entt::registry& reg = things_.registry();
    const int cx = rect.minX + rect.sizeX / 2;
    const int cy = rect.minY + rect.sizeY / 2;
    std::vector<CellPos> ring;
    const auto inside = [&rect](CellPos c) { return rect.contains(c); };

    if (blocksMovement) {
        std::vector<Entity> pawns;
        reg.view<const Position, const Mover>().each([&](Entity pawn, const Position& pos, const Mover&) {
            if (inside(cellOf(pos))) {
                pawns.push_back(pawn);
            }
        });
        std::sort(pawns.begin(), pawns.end());
        for (const Entity pawn : pawns) {
            for (int r = 0; r <= kConstructionDropRadius + std::max(rect.sizeX, rect.sizeY); ++r) {
                constructionRing(cx, cy, r, rect.level, ring);
                const auto free = std::find_if(ring.begin(), ring.end(), [&](const CellPos& c) {
                    return !inside(c) && map_.walkable(c);
                });
                if (free != ring.end()) {
                    Position& pos = reg.get<Position>(pawn);
                    pos.x = static_cast<float>(free->x) + 0.5f;
                    pos.y = static_cast<float>(free->y) + 0.5f;
                    Mover& mover = reg.get<Mover>(pawn);
                    // A walk cut short is given up, so the job asks for a new path from here instead
                    // of taking the empty route for arriving.
                    mover.gaveUp = mover.gaveUp || !mover.arrived();
                    mover.path.clear();
                    mover.next = 0;
                    mover.vx = 0.0f;
                    mover.vy = 0.0f;
                    break;
                }
            }
        }
    }

    if (!standable) {
        std::vector<Entity> items;
        for (int y = rect.minY; y < rect.minY + rect.sizeY; ++y) {
            for (int x = rect.minX; x < rect.minX + rect.sizeX; ++x) {
                const std::vector<Entity>& there = things_.itemsAt(CellPos{x, y, rect.level});
                items.insert(items.end(), there.begin(), there.end());
            }
        }
        std::sort(items.begin(), items.end());
        for (const Entity item : items) {
            // The same stack moves to the nearest empty ground outside, so a pawn on its way to it (or
            // carrying it to a site) still has it; with no empty cell near, it is made again by dropNear.
            bool moved = false;
            for (int r = 0; r <= kConstructionDropRadius + std::max(rect.sizeX, rect.sizeY) && !moved; ++r) {
                constructionRing(cx, cy, r, rect.level, ring);
                for (const CellPos& c : ring) {
                    const Entity building = map_.inBounds(c) ? map_.buildingAt(c) : kNoEntity;
                    if (inside(c) || !map_.walkable(c) || things_.itemsAt(c).size() != 0 ||
                        (building != kNoEntity && defs_.get(reg.get<Thing>(building).def).text("passability") != "Standable")) {
                        continue;
                    }
                    things_.despawn(item);
                    moved = things_.spawnAt(item, c);
                    if (moved) {
                        break;
                    }
                }
            }
            if (!moved) {
                const DefId def = reg.get<Thing>(item).def;
                const int count = reg.get<StackCount>(item).count;
                things_.destroy(item);
                dropNear(def, count, rect, true);
            }
        }
    }
}

Entity Construction::complete(Entity frame) {
    entt::registry& reg = things_.registry();
    const ConstructionSite* found = reg.valid(frame) ? reg.try_get<ConstructionSite>(frame) : nullptr;
    if (found == nullptr) {
        return kNoEntity;
    }
    const ConstructionSite site = *found;
    const DefId def = reg.get<Thing>(frame).def;
    const Def& d = defs_.get(def);
    const CellRect rect = site.rect();
    const bool isDoor = d.text("thingClass") == "Building_Door";
    const bool blocks = !isDoor && d.text("passability") == "Impassable";
    const bool standable = d.text("passability") == "Standable";

    indexSite(frame, false);
    things_.destroy(frame);
    clearFootprint(frame, rect, blocks, standable);
    const Entity building = things_.spawn(def, site.origin, 1, site.rotation);
    if (building != kNoEntity) {
        // Plants don't grow under a building (docs/design/plants.md).
        for (int y = rect.minY; y < rect.minY + rect.sizeY; ++y) {
            for (int x = rect.minX; x < rect.minX + rect.sizeX; ++x) {
                plants_.remove(CellPos{x, y, rect.level});
            }
        }
    }
    if (building == kNoEntity) {
        for (const MaterialCount& m : site.materials) {
            dropNear(m.item, m.have, rect);
        }
        return kNoEntity;
    }
    const int hp = maxHitPoints(def, site.stuff);
    if (hp > 0) {
        reg.emplace_or_replace<HitPoints>(building, HitPoints{hp, hp});
    }
    if (site.stuff.valid()) {
        reg.emplace<BuiltOf>(building, BuiltOf{site.stuff});
    }
    return building;
}

float Construction::deconstructWork(Entity building) const {
    const entt::registry& reg = things_.registry();
    const DefId def = reg.get<Thing>(building).def;
    const BuiltOf* made = reg.try_get<BuiltOf>(building);
    const double work = workToBuild(def, made ? made->stuff : DefId{});
    return static_cast<float>(std::clamp(work, numbers_.deconstructWorkMin, numbers_.deconstructWorkMax));
}

bool Construction::deconstruct(Entity building) {
    entt::registry& reg = things_.registry();
    if (!things_.spawned(building) || !reg.all_of<Thing, Footprint>(building) || reg.all_of<ConstructionSite>(building)) {
        return false;
    }
    const DefId def = reg.get<Thing>(building).def;
    const BuiltOf* made = reg.try_get<BuiltOf>(building);
    const std::vector<Cost> costs = costOf(def, made ? made->stuff : DefId{});
    const CellRect rect = reg.get<Footprint>(building).rect();
    std::vector<Cost> refund;
    for (const Cost& cost : costs) {
        // RimWorld rounds a half at random: 5 give back 2 or 3.
        const double exact = static_cast<double>(cost.count) * numbers_.deconstructRefundFactor;
        int count = static_cast<int>(std::floor(exact));
        const double fraction = exact - static_cast<double>(count);
        if (fraction > 0.0 && static_cast<double>(rng_.unit()) < fraction) {
            ++count;
        }
        refund.push_back(Cost{cost.item, count});
    }
    things_.destroy(building);
    for (const Cost& cost : refund) {
        dropNear(cost.item, cost.count, rect);
    }
    return true;
}

void Construction::dropNear(DefId item, int count, const CellRect& rect, bool skipRect) {
    if (count <= 0) {
        return;
    }
    entt::registry& reg = things_.registry();
    const int limit = stackLimit(item);
    const int cx = rect.minX + rect.sizeX / 2;
    const int cy = rect.minY + rect.sizeY / 2;
    const int reach = kConstructionDropRadius + std::max(rect.sizeX, rect.sizeY);
    std::vector<CellPos> ring;
    int left = count;
    for (int r = 0; r <= reach && left > 0; ++r) {
        constructionRing(cx, cy, r, rect.level, ring);
        for (const CellPos& c : ring) {
            if (left <= 0) {
                break;
            }
            if (skipRect && rect.contains(c)) {
                continue;
            }
            // Cells of a building that blocks (a wall still standing) or that can't hold items
            // (a table, a stove) are no place for them.
            const Entity building = map_.inBounds(c) ? map_.buildingAt(c) : kNoEntity;
            if (!map_.walkable(c) || (building != kNoEntity && defs_.get(reg.get<Thing>(building).def).text("passability") != "Standable")) {
                continue;
            }
            const std::vector<Entity>& there = things_.itemsAt(c);
            if (there.empty()) {
                const int put = std::min(left, limit);
                if (things_.spawn(item, c, put) != kNoEntity) {
                    left -= put;
                }
            } else if (there.size() == 1 && reg.get<Thing>(there.front()).def == item) {
                StackCount& stack = reg.get<StackCount>(there.front());
                const int put = std::min(left, limit - stack.count);
                if (put > 0) {
                    stack.count += put;
                    left -= put;
                }
            }
        }
    }
}

// ---- Where pawns stand and how fast they work -------------------------------------------------

bool Construction::workCell(const CellRect& rect, CellPos from, CellPos& out) const {
    bool found = false;
    float best = 0.0f;
    const auto consider = [&](int x, int y) {
        const CellPos c{x, y, rect.level};
        // Not on a planned building's cells: it may be finished while the pawn stands there.
        if (!map_.walkable(c) || siteAt(c) != kNoEntity || !map_.regions().canReach(from, c)) {
            return;
        }
        const float dx = static_cast<float>(x - from.x);
        const float dy = static_cast<float>(y - from.y);
        const float dist = dx * dx + dy * dy;
        if (!found || dist < best) {
            found = true;
            best = dist;
            out = c;
        }
    };
    for (int y = rect.minY; y < rect.minY + rect.sizeY; ++y) {
        consider(rect.minX - 1, y);
        consider(rect.minX + rect.sizeX, y);
    }
    for (int x = rect.minX; x < rect.minX + rect.sizeX; ++x) {
        consider(x, rect.minY - 1);
        consider(x, rect.minY + rect.sizeY);
    }
    return found;
}

float Construction::skillSpeed(Entity pawn) const {
    int level = numbers_.defaultSkillLevel;
    if (const Colonist* colonist = things_.registry().try_get<Colonist>(pawn)) {
        if (const SkillRecord* skill = colonist->skill(kConstructionSkill)) {
            level = skill->level;
        }
    }
    if (const PawnGenes* genes = things_.registry().try_get<PawnGenes>(pawn)) {
        level = std::max(0, level + geneAptitude(*genes, kConstructionSkill));
    }
    return static_cast<float>(std::max(0.0, numbers_.skillSpeedBase + numbers_.skillSpeedPerLevel * level));
}

bool Construction::afterLoad(std::string& error) {
    siteAt_.clear();
    entt::registry& reg = things_.registry();
    std::vector<Entity> sites;
    for (const Entity e : reg.view<ConstructionSite>()) {
        sites.push_back(e);
    }
    std::sort(sites.begin(), sites.end());
    for (const Entity e : sites) {
        const CellRect rect = reg.get<ConstructionSite>(e).rect();
        for (int y = rect.minY; y < rect.minY + rect.sizeY; ++y) {
            for (int x = rect.minX; x < rect.minX + rect.sizeX; ++x) {
                const CellPos c{x, y, rect.level};
                if (!map_.inBounds(c)) {
                    error = "the save has a building plan off the map";
                    return false;
                }
                if (!siteAt_.emplace(constructionCellKey(c), e).second) {
                    error = "the save has two building plans on the same cell";
                    return false;
                }
            }
        }
    }
    return true;
}

}  // namespace colony
