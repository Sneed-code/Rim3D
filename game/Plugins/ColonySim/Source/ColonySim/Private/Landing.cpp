// Shuttles and the landing zone (docs/design/landing.md).
#include "ColonySim/Landing.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Map.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Random.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace colony {

namespace {

constexpr int kLandingAsideRadius = 12;  // cells round the footprint a pawn may be moved to

}  // namespace

const char* shuttlePhaseName(ShuttlePhase phase) {
    switch (phase) {
        case ShuttlePhase::Descending: return "Descending";
        case ShuttlePhase::Landed: return "Landed";
        case ShuttlePhase::Ascending: return "Ascending";
    }
    return "?";
}

LandingRules LandingRules::read(const DefDatabase& defs) {
    LandingRules r;
    if (const Def* d = defs.find("Rules", "LandingRules")) {
        r.present = true;
        r.shuttleDef = d->text("shuttleDef", r.shuttleDef);
        r.descendSteps = std::max(1, static_cast<int>(d->number("descendSteps", r.descendSteps)));
        r.ascendSteps = std::max(1, static_cast<int>(d->number("ascendSteps", r.ascendSteps)));
        r.maxSlopeM = static_cast<float>(d->number("maxSlopeM", r.maxSlopeM));
        r.searchMinM = static_cast<float>(d->number("searchRadiusM.0", r.searchMinM));
        r.searchMaxM = std::max(r.searchMinM, static_cast<float>(d->number("searchRadiusM.1", r.searchMaxM)));
        r.searchTries = std::max(1, static_cast<int>(d->number("searchTries", r.searchTries)));
        r.visitShuttleChance = std::clamp(static_cast<float>(d->number("visitShuttleChance", r.visitShuttleChance)), 0.0f, 1.0f);
    }
    return r;
}

LandingSystem::LandingSystem(Things& things, Map& map, Plants& plants, const DefDatabase& defs, const GameTime& time, Rng& rng)
    : things_(things), map_(map), plants_(plants), defs_(defs), time_(time), rng_(rng), rules_(LandingRules::read(defs)) {
    if (rules_.present) {
        if (const Def* d = defs.find("Things", rules_.shuttleDef)) {
            shuttleDef_ = d->id();
            sizeX_ = std::max(1, static_cast<int>(d->number("size.0", sizeX_)));
            sizeY_ = std::max(1, static_cast<int>(d->number("size.1", sizeY_)));
        }
    }
}

int64_t LandingSystem::steps() const { return time_.steps; }

// ---- The landing zone -------------------------------------------------------------------------

int LandingSystem::paintZone(const CellRect& rect, bool add) {
    int changed = 0;
    for (int y = rect.minY; y < rect.minY + rect.sizeY; ++y) {
        for (int x = rect.minX; x < rect.minX + rect.sizeX; ++x) {
            const CellPos c{x, y, 0};
            if (!map_.onGround(c)) {
                continue;
            }
            const int index = map_.index(c);
            if (add ? zone_.insert(index).second : zone_.erase(index) > 0) {
                ++changed;
            }
        }
    }
    if (changed > 0) {
        ++zoneChanges_;
    }
    return changed;
}

bool LandingSystem::inZone(CellPos c) const { return map_.onGround(c) && zone_.count(map_.index(c)) > 0; }

std::vector<CellPos> LandingSystem::zoneCells() const {
    std::vector<CellPos> cells;
    cells.reserve(zone_.size());
    for (const int index : zone_) {
        cells.push_back(map_.cellAt(index));
    }
    return cells;
}

// ---- Where a shuttle can set down ------------------------------------------------------------

CellPos LandingSystem::local(CellPos origin, Rot4 rot, int lx, int ly) const {
    switch (rot) {
        case Rot4::North: return CellPos{origin.x + lx, origin.y + ly, 0};
        case Rot4::East: return CellPos{origin.x + ly, origin.y + (sizeX_ - 1 - lx), 0};
        case Rot4::South: return CellPos{origin.x + (sizeX_ - 1 - lx), origin.y + (sizeY_ - 1 - ly), 0};
        case Rot4::West: return CellPos{origin.x + (sizeY_ - 1 - ly), origin.y + lx, 0};
    }
    return origin;
}

bool LandingSystem::siteClear(CellPos origin, Rot4 rot, bool allowPawns) const {
    const CellRect rect = footprintRect(CellPos{origin.x, origin.y, 0}, sizeX_, sizeY_, rot);
    float low = std::numeric_limits<float>::max();
    float high = std::numeric_limits<float>::lowest();
    for (int y = rect.minY; y < rect.minY + rect.sizeY; ++y) {
        for (int x = rect.minX; x < rect.minX + rect.sizeX; ++x) {
            const CellPos c{x, y, 0};
            // Hard things stop it: off the map, deep water, rock, any building, a roof or upper floor, items, trees.
            if (!map_.onGround(c) || !map_.groundWalkable(c) || map_.buildingAt(c) != kNoEntity || map_.roof(c) != Roof::None ||
                map_.floorAbove(c) || !things_.itemsAt(c).empty()) {
                return false;
            }
            if (plants_.has(c) && defs_.get(plants_.defAt(c)).flag("plant.isTree")) {
                return false;
            }
            const float h = map_.groundHeight(c);
            low = std::min(low, h);
            high = std::max(high, h);
        }
    }
    if (high - low > rules_.maxSlopeM) {
        return false;
    }
    // People must be able to step out of the door.
    const CellPos d = local(origin, rot, -1, sizeY_ / 2 + 1);
    if (!map_.walkable(d) || map_.isDoor(d)) {
        return false;
    }
    if (!allowPawns) {
        bool pawn = false;
        things_.registry().view<const Position, const Mover>().each([&](const Position& pos, const Mover&) {
            pawn = pawn || rect.contains(cellOf(pos));
        });
        if (pawn) {
            return false;
        }
    }
    return true;
}

bool LandingSystem::findSite(float nearX, float nearY, CellPos& origin, Rot4& rot) const {
    if (!available()) {
        return false;
    }
    constexpr Rot4 kRots[4] = {Rot4::North, Rot4::East, Rot4::South, Rot4::West};
    if (!zone_.empty()) {
        // The zone's middle, then every corner in it whose whole footprint lies in the zone; the one nearest the middle.
        double sx = 0.0;
        double sy = 0.0;
        for (const int index : zone_) {
            const CellPos c = map_.cellAt(index);
            sx += c.x + 0.5;
            sy += c.y + 0.5;
        }
        const double mx = sx / static_cast<double>(zone_.size());
        const double my = sy / static_cast<double>(zone_.size());
        double best = std::numeric_limits<double>::max();
        bool found = false;
        for (const int index : zone_) {
            const CellPos c = map_.cellAt(index);
            for (const Rot4 r : kRots) {
                // North and South (East and West) cover the same cells; they differ in which side the door is on.
                const CellRect rect = footprintRect(c, sizeX_, sizeY_, r);
                bool inside = true;
                for (int y = rect.minY; y < rect.minY + rect.sizeY && inside; ++y) {
                    for (int x = rect.minX; x < rect.minX + rect.sizeX && inside; ++x) {
                        inside = inZone(CellPos{x, y, 0});
                    }
                }
                if (!inside) {
                    continue;
                }
                const double cx = rect.minX + rect.sizeX * 0.5 - mx;
                const double cy = rect.minY + rect.sizeY * 0.5 - my;
                const double d = cx * cx + cy * cy;
                if (d < best && siteClear(c, r, true)) {
                    best = d;
                    origin = c;
                    rot = r;
                    found = true;
                }
            }
        }
        if (found) {
            return true;
        }
    }
    // No zone, or no room in it: a clear spot at random round the colony, then anywhere on the map.
    for (int attempt = 0; attempt < rules_.searchTries * 2; ++attempt) {
        float cx = 0.0f;
        float cy = 0.0f;
        if (attempt < rules_.searchTries) {
            const float angle = rng_.unit() * 6.2831853f;
            const float dist = rules_.searchMinM + rng_.unit() * (rules_.searchMaxM - rules_.searchMinM);
            cx = nearX + std::cos(angle) * dist;
            cy = nearY + std::sin(angle) * dist;
        } else {
            cx = rng_.unit() * static_cast<float>(map_.width());
            cy = rng_.unit() * static_cast<float>(map_.height());
        }
        const Rot4 r = kRots[rng_.below(4)];
        const CellRect size = footprintRect(CellPos{}, sizeX_, sizeY_, r);
        const CellPos c{static_cast<int>(std::floor(cx)) - size.sizeX / 2, static_cast<int>(std::floor(cy)) - size.sizeY / 2, 0};
        if (siteClear(c, r, false)) {
            origin = c;
            rot = r;
            return true;
        }
    }
    return false;
}

// ---- Shuttles ----------------------------------------------------------------------------------

void LandingSystem::flattenPlants(const CellRect& rect) {
    for (int y = rect.minY; y < rect.minY + rect.sizeY; ++y) {
        for (int x = rect.minX; x < rect.minX + rect.sizeX; ++x) {
            const CellPos c{x, y, 0};
            if (map_.onGround(c) && plants_.has(c)) {
                plants_.remove(c);
            }
        }
    }
}

void LandingSystem::moveAside(const CellRect& rect) {
    entt::registry& reg = things_.registry();
    std::vector<Entity> pawns;
    reg.view<const Position, const Mover>().each([&](Entity pawn, const Position& pos, const Mover&) {
        if (rect.contains(cellOf(pos))) {
            pawns.push_back(pawn);
        }
    });
    std::sort(pawns.begin(), pawns.end());
    for (const Entity pawn : pawns) {
        const CellPos here = cellOf(reg.get<Position>(pawn));
        // The nearest walkable cell outside the footprint, ring by ring round the pawn.
        bool moved = false;
        for (int r = 1; r <= kLandingAsideRadius + std::max(rect.sizeX, rect.sizeY) && !moved; ++r) {
            for (int dy = -r; dy <= r && !moved; ++dy) {
                for (int dx = -r; dx <= r && !moved; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dy)) != r) {
                        continue;
                    }
                    const CellPos c{here.x + dx, here.y + dy, 0};
                    if (rect.contains(c) || !map_.walkable(c)) {
                        continue;
                    }
                    Position& pos = reg.get<Position>(pawn);
                    pos.x = static_cast<float>(c.x) + 0.5f;
                    pos.y = static_cast<float>(c.y) + 0.5f;
                    Mover& mover = reg.get<Mover>(pawn);
                    mover.gaveUp = mover.gaveUp || !mover.arrived();
                    mover.path.clear();
                    mover.next = 0;
                    mover.vx = 0.0f;
                    mover.vy = 0.0f;
                    moved = true;
                }
            }
        }
    }
}

bool LandingSystem::land(uint32_t visit, std::vector<ShuttlePassenger> passengers, float nearX, float nearY, uint32_t& id,
                         std::string& error) {
    if (!available()) {
        error = "there is no shuttle in the data files";
        return false;
    }
    CellPos origin;
    Rot4 rot = Rot4::North;
    if (!findSite(nearX, nearY, origin, rot)) {
        error = "nowhere clear for a shuttle to land";
        return false;
    }
    const CellRect rect = footprintRect(origin, sizeX_, sizeY_, rot);
    // It claims the footprint now: those standing there step aside, and nobody builds or walks into it.
    moveAside(rect);
    const Entity building = things_.spawn(shuttleDef_, origin, 1, rot);
    if (building == kNoEntity) {
        error = "the shuttle couldn't be placed";
        return false;
    }
    Shuttle s;
    s.id = nextId_++;
    s.building = building;
    s.origin = origin;
    s.rot = rot;
    s.phase = ShuttlePhase::Descending;
    s.phaseStep = steps();
    s.visit = visit;
    s.passengers = std::move(passengers);
    id = s.id;
    shuttles_.push_back(std::move(s));
    return true;
}

bool LandingSystem::depart(uint32_t id) {
    for (Shuttle& s : shuttles_) {
        if (s.id == id) {
            if (s.phase != ShuttlePhase::Landed) {
                return false;
            }
            s.phase = ShuttlePhase::Ascending;
            s.phaseStep = steps();
            return true;
        }
    }
    return false;
}

const Shuttle* LandingSystem::find(uint32_t id) const {
    for (const Shuttle& s : shuttles_) {
        if (s.id == id) {
            return &s;
        }
    }
    return nullptr;
}

const Shuttle* LandingSystem::ofVisit(uint32_t visit) const {
    if (visit == 0) {
        return nullptr;
    }
    for (const Shuttle& s : shuttles_) {
        if (s.visit == visit) {
            return &s;
        }
    }
    return nullptr;
}

bool LandingSystem::awaiting(uint32_t visit) const {
    const Shuttle* s = ofVisit(visit);
    return s != nullptr && s->phase == ShuttlePhase::Descending;
}

CellPos LandingSystem::door(const Shuttle& s) const { return local(s.origin, s.rot, -1, sizeY_ / 2 + 1); }

CellRect LandingSystem::footprint(const Shuttle& s) const { return footprintRect(s.origin, sizeX_, sizeY_, s.rot); }

float LandingSystem::centreX(const Shuttle& s) const {
    const CellRect r = footprint(s);
    return static_cast<float>(r.minX) + static_cast<float>(r.sizeX) * 0.5f;
}

float LandingSystem::centreY(const Shuttle& s) const {
    const CellRect r = footprint(s);
    return static_cast<float>(r.minY) + static_cast<float>(r.sizeY) * 0.5f;
}

float LandingSystem::progress(const Shuttle& s) const {
    const int64_t length = s.phase == ShuttlePhase::Descending ? rules_.descendSteps : s.phase == ShuttlePhase::Ascending ? rules_.ascendSteps : 1;
    if (s.phase == ShuttlePhase::Landed) {
        return 1.0f;
    }
    return std::clamp(static_cast<float>(steps() - s.phaseStep) / static_cast<float>(length), 0.0f, 1.0f);
}

void LandingSystem::step() {
    if (shuttles_.empty()) {
        return;
    }
    const int64_t now = steps();
    for (size_t i = 0; i < shuttles_.size();) {
        Shuttle& s = shuttles_[i];
        if (!things_.spawned(s.building)) {
            // Destroyed where it stood (or taken off by something else): it is gone, and anyone aboard with it.
            shuttles_.erase(shuttles_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (s.phase == ShuttlePhase::Descending && now - s.phaseStep >= rules_.descendSteps) {
            s.phase = ShuttlePhase::Landed;
            s.phaseStep = now;
            flattenPlants(footprint(s));
            if (onLanded_) {
                onLanded_(s);
            }
            s.passengers.clear();
        } else if (s.phase == ShuttlePhase::Ascending && now - s.phaseStep >= rules_.ascendSteps) {
            things_.destroy(s.building);
            shuttles_.erase(shuttles_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

// ---- Saves -------------------------------------------------------------------------------------

LandingSaved LandingSystem::saved() const {
    LandingSaved out;
    out.nextId = nextId_;
    out.zone.assign(zone_.begin(), zone_.end());
    out.shuttles = shuttles_;
    return out;
}

bool LandingSystem::restore(const LandingSaved& saved, std::string& error) {
    const int cells = map_.width() * map_.height();
    for (const int index : saved.zone) {
        if (index < 0 || index >= cells) {
            error = "the save has a landing zone cell off the map";
            return false;
        }
    }
    uint32_t last = 0;
    for (const Shuttle& s : saved.shuttles) {
        if (s.id == 0 || s.id <= last || s.id >= saved.nextId || static_cast<int>(s.phase) > static_cast<int>(ShuttlePhase::Ascending) ||
            static_cast<int>(s.rot) > static_cast<int>(Rot4::West)) {
            error = "the save has a shuttle that doesn't add up";
            return false;
        }
        last = s.id;
    }
    zone_ = std::set<int>(saved.zone.begin(), saved.zone.end());
    shuttles_ = saved.shuttles;
    nextId_ = saved.nextId;
    ++zoneChanges_;
    return true;
}

void LandingSystem::hashState(StateHasher& h) const {
    h.add(nextId_);
    h.add(static_cast<uint64_t>(zone_.size()));
    for (const int index : zone_) {
        h.add(index);
    }
    for (const Shuttle& s : shuttles_) {
        h.add(s.id);
        h.add(static_cast<uint64_t>(entt::to_integral(s.building)));
        h.add(s.origin.x);
        h.add(s.origin.y);
        h.add(static_cast<int>(s.rot));
        h.add(static_cast<int>(s.phase));
        h.add(s.phaseStep);
        h.add(s.visit);
        h.add(static_cast<uint64_t>(s.passengers.size()));
        for (const ShuttlePassenger& p : s.passengers) {
            h.add(p.kind.data(), p.kind.size());
            h.add(static_cast<int>(p.role));
        }
    }
}

}  // namespace colony
