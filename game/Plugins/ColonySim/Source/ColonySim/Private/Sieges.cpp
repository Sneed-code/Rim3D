#include "ColonySim/Raids.h"

#include "ColonySim/Artillery.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Fuel.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Random.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace colony {

// Sieges (docs/design/sieges.md): a raid that walks in, digs in out of the colony's reach, has its shells dropped in,
// builds mortars and sandbags, shells the colony from its camp, and assaults when its supplies run out, it has lost
// too many, its mortars are gone or it has sat there too long. The rest of a raid (morale, rout, the alarm) is Raids.cpp.

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.
float siegeCentre(int v) {
    return static_cast<float>(v) + 0.5f;
}

float siegeDist(float ax, float ay, float bx, float by) {
    return std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by));
}

// Metres from a point to the nearest edge of a rect of cells (0 inside it).
float siegeDistToRect(float x, float y, const CellRect& r) {
    const float dx = std::max({static_cast<float>(r.minX) - x, 0.0f, x - static_cast<float>(r.minX + r.sizeX)});
    const float dy = std::max({static_cast<float>(r.minY) - y, 0.0f, y - static_cast<float>(r.minY + r.sizeY)});
    return std::sqrt(dx * dx + dy * dy);
}

bool siegeInRect(const CellRect& r, CellPos c) {
    return c.level == r.level && c.x >= r.minX && c.y >= r.minY && c.x < r.minX + r.sizeX && c.y < r.minY + r.sizeY;
}

constexpr float kSiegePi = 3.14159265358979f;

}  // namespace

CellRect RaidSystem::buildRect(const SiegeBuild& build) const {
    const Def* d = defs_.find("Things", build.def);
    const int sx = d != nullptr ? std::max(1, static_cast<int>(d->numberAt("size", 0, 1.0))) : 1;
    const int sy = d != nullptr ? std::max(1, static_cast<int>(d->numberAt("size", 1, 1.0))) : 1;
    return footprintRect(build.origin, sx, sy, build.rotation);
}

// The camp: on open, unroofed ground the raid can walk to, campDistanceMax down to campDistanceMin from the colony,
// on the entry's side if it can. Its mortars stand a little behind its middle, side by side across the line to the
// colony; lines of sandbags in front of it, toward the colony.
bool RaidSystem::planSiege(Siege& siege, float points, CellPos entry, CellPos target, std::string& error) {
    if (construction_ == nullptr || artillery_ == nullptr) {
        error = "a siege can't be laid on this map";
        return false;
    }
    const DefId mortar = defs_.idOf("Things", rules_.siegeMortar);
    const DefId cover = defs_.idOf("Things", rules_.siegeCover);
    const DefId stuff = rules_.siegeCoverStuff.empty() ? DefId{} : defs_.idOf("Things", rules_.siegeCoverStuff);
    if (!mortar.valid() || !artillery_->isArtillery(mortar) || !construction_->buildable(mortar) || !cover.valid() ||
        !construction_->buildable(cover) || !construction_->stuffFits(cover, stuff) || !defs_.idOf("Things", rules_.siegeShell).valid()) {
        error = "the siege's mortar, sandbags or shells are missing from the defs";
        return false;
    }
    const Def& md = defs_.get(mortar);
    const int msx = std::max(1, static_cast<int>(md.numberAt("size", 0, 1.0)));
    const int msy = std::max(1, static_cast<int>(md.numberAt("size", 1, 1.0)));
    std::vector<CellPos> taken;  // cells the camp's builds (and the room round its mortars) take
    const auto open = [&](CellPos c) {
        return map_.onGround(c) && map_.walkable(c) && map_.buildingAt(c) == kNoEntity && construction_->siteAt(c) == kNoEntity &&
               map_.roof(c) == Roof::None && std::find(taken.begin(), taken.end(), c) == taken.end();
    };

    Regions& regions = map_.regions();
    const float tx = siegeCentre(target.x);
    const float ty = siegeCentre(target.y);
    const float base = std::atan2(siegeCentre(entry.y) - ty, siegeCentre(entry.x) - tx);
    const int margin = rules_.edgeDepth + 4;
    bool found = false;
    CellPos camp;
    for (float d = rules_.campDistanceMax; d >= rules_.campDistanceMin - 0.01f && !found; d -= 5.0f) {
        for (int k = 0; k < 24 && !found; ++k) {
            // Turning away from the entry's side 15 degrees at a time, one way then the other.
            const float turn = static_cast<float>((k + 1) / 2) * (k % 2 == 1 ? 1.0f : -1.0f) * (kSiegePi / 12.0f);
            const CellPos c{static_cast<int>(std::floor(tx + d * std::cos(base + turn))), static_cast<int>(std::floor(ty + d * std::sin(base + turn))), 0};
            if (c.x < margin || c.y < margin || c.x >= map_.width() - margin || c.y >= map_.height() - margin) {
                continue;
            }
            bool fits = true;
            for (int dy = -3; dy <= 3 && fits; ++dy) {
                for (int dx = -3; dx <= 3 && fits; ++dx) {
                    fits = open(CellPos{c.x + dx, c.y + dy, 0});
                }
            }
            if (fits && regions.canReach(entry, c)) {
                camp = c;
                found = true;
            }
        }
    }
    if (!found) {
        error = "no open ground for a siege camp within mortar range of the colony";
        return false;
    }

    // Toward the colony, and across that line.
    float ux = tx - siegeCentre(camp.x);
    float uy = ty - siegeCentre(camp.y);
    const float length = std::max(0.001f, std::sqrt(ux * ux + uy * uy));
    ux /= length;
    uy /= length;
    const float px = -uy;
    const float py = ux;
    const auto cellAt = [&](float along, float side) {
        return CellPos{static_cast<int>(std::floor(siegeCentre(camp.x) + ux * along + px * side)),
                       static_cast<int>(std::floor(siegeCentre(camp.y) + uy * along + py * side)), 0};
    };

    const int mortars = std::clamp(static_cast<int>(std::lround(points / rules_.pointsPerMortar)), 1, rules_.mortarsMax);
    for (int i = 0; i < mortars; ++i) {
        const float side = (static_cast<float>(i) - static_cast<float>(mortars - 1) * 0.5f) * 6.0f;
        bool placed = false;
        for (int shift = 0; shift <= 3 && !placed; ++shift) {
            for (const float sign : {1.0f, -1.0f}) {
                const CellPos centre = cellAt(-1.0f, side + sign * static_cast<float>(shift));
                const CellPos origin{centre.x - (msx - 1) / 2, centre.y - (msy - 1) / 2, 0};
                const CellRect r = footprintRect(origin, msx, msy, Rot4::North);
                bool fits = true;
                for (int y = r.minY; y < r.minY + r.sizeY && fits; ++y) {
                    for (int x = r.minX; x < r.minX + r.sizeX && fits; ++x) {
                        fits = open(CellPos{x, y, 0});
                    }
                }
                if (fits) {
                    // The gun and a ring round it, where its crew and builders stand, are kept clear of sandbags.
                    for (int y = r.minY - 1; y <= r.minY + r.sizeY; ++y) {
                        for (int x = r.minX - 1; x <= r.minX + r.sizeX; ++x) {
                            taken.push_back(CellPos{x, y, 0});
                        }
                    }
                    SiegeBuild b;
                    b.def = rules_.siegeMortar;
                    b.origin = origin;
                    b.workLeft = construction_->workToBuild(mortar, DefId{});
                    siege.builds.push_back(std::move(b));
                    placed = true;
                    break;
                }
                if (shift == 0) {
                    break;  // no other side to try
                }
            }
        }
    }
    if (siege.builds.empty()) {
        error = "no room for the siege's mortars";
        return false;
    }

    const int lines = rules_.coverLinesMin + static_cast<int>(rng_.below(static_cast<uint32_t>(rules_.coverLinesMax - rules_.coverLinesMin + 1)));
    const float coverWork = construction_->workToBuild(cover, stuff);
    for (int l = 0; l < lines; ++l) {
        const int count = rules_.coverLengthMin + static_cast<int>(rng_.below(static_cast<uint32_t>(rules_.coverLengthMax - rules_.coverLengthMin + 1)));
        const float along = 4.0f + static_cast<float>(rng_.below(3));       // 4 to 6 m toward the colony
        const float side = static_cast<float>(rng_.below(13)) - 6.0f;       // -6 to 6 m across
        for (int j = 0; j < count; ++j) {
            const CellPos c = cellAt(along, side + static_cast<float>(j) - static_cast<float>(count) * 0.5f);
            if (!open(c)) {
                continue;
            }
            taken.push_back(c);
            SiegeBuild b;
            b.def = rules_.siegeCover;
            b.stuff = rules_.siegeCoverStuff;
            b.origin = c;
            b.workLeft = coverWork;
            siege.builds.push_back(std::move(b));
        }
    }
    siege.stage = SiegeStage::March;
    siege.camp = camp;
    return true;
}

// Every squad think (half a minute) before the assault: whether to go in, the supply drops, who mans, builds and
// guards, and what the mortars shell. Once it assaults it is an Assault squad, that digs through walls in its way.
void RaidSystem::thinkSiege(Squad& squad, float lost) {
    Siege& s = squad.siege;
    if (s.stage == SiegeStage::Assault) {
        thinkBreach(squad);
        return;
    }
    const entt::registry& reg = things_.registry();
    int up = 0;       // mortars standing
    int planned = 0;  // mortars not built yet
    for (const SiegeBuild& b : s.builds) {
        if (b.def != rules_.siegeMortar) {
            continue;
        }
        if (!b.done) {
            ++planned;
        } else if (b.built != kNoEntity && things_.spawned(b.built)) {
            ++up;
        }
    }
    if (lost > 0.0f && lost >= rules_.siegeAssaultLost * static_cast<float>(squad.started)) {
        startAssault(squad, "losses");
        return;
    }
    if (now() >= s.endsAt) {
        startAssault(squad, "time");
        return;
    }
    if (up == 0 && planned == 0) {
        startAssault(squad, "no mortars");
        return;
    }
    if (s.stage == SiegeStage::Build && planned == 0) {
        s.stage = SiegeStage::Shell;
        siegeEvent(squad, "shelling");
    }
    if (s.stage == SiegeStage::Shell) {
        const int shells = siegeShells(squad, nullptr);
        if (shells < rules_.resupplyAt && s.resupplies < rules_.resupplyMax) {
            ++s.resupplies;
            dropShells(squad, rules_.resupplyShells);
        } else if (shells == 0 && s.resupplies >= rules_.resupplyMax) {
            startAssault(squad, "out of shells");
            return;
        }
    }
    siegeOrders(squad);
    if (s.stage == SiegeStage::Shell) {
        for (const SiegeBuild& b : s.builds) {
            if (b.built != kNoEntity && things_.spawned(b.built) && reg.all_of<Artillery>(b.built) && artillery_->manned(b.built)) {
                siegeAim(squad, b.built);
            }
        }
    }
}

// Who does what: on the march, everyone walks to the camp. Then a crew for every standing mortar (kept while it can,
// else the last free member), builders (the first builderShare of the rest) at the next thing to build, the others
// guarding the camp. An enemy near the camp sends the builders to fight; the crews stay at their guns.
void RaidSystem::siegeOrders(Squad& squad) {
    entt::registry& reg = things_.registry();
    Siege& s = squad.siege;
    std::vector<Entity> able;
    for (const Entity m : squad.members) {
        if (things_.spawned(m) && !health_.incapacitated(m) && reg.all_of<Fighter, Raider>(m) && reg.get<Raider>(m).loot == kNoEntity) {
            able.push_back(m);
        }
    }
    if (able.empty()) {
        return;
    }
    const bool marching = s.stage == SiegeStage::March;
    const auto in = [](const std::vector<Entity>& list, Entity e) { return std::find(list.begin(), list.end(), e) != list.end(); };

    std::vector<Entity> crews;
    std::vector<Entity> guns;  // crews[i] mans guns[i]
    if (!marching) {
        for (const SiegeBuild& b : s.builds) {
            if (b.built == kNoEntity || !things_.spawned(b.built) || !reg.all_of<Artillery>(b.built)) {
                continue;
            }
            const Entity crew = reg.get<Artillery>(b.built).crew;
            if (crew != kNoEntity && reg.valid(crew) && !health_.incapacitated(crew) && fighters_.factionOf(crew) == kPlayerFaction) {
                continue;  // a colonist took the gun
            }
            Entity pick = crew != kNoEntity && in(able, crew) && !in(crews, crew) ? crew : kNoEntity;
            for (auto it = able.rbegin(); it != able.rend() && pick == kNoEntity; ++it) {
                if (!in(crews, *it)) {
                    pick = *it;
                }
            }
            if (pick == kNoEntity) {
                break;
            }
            if (pick != crew) {
                artillery_->setForeignCrew(b.built, pick);
            }
            crews.push_back(pick);
            guns.push_back(b.built);
        }
    }

    bool attacked = false;
    const float cx = siegeCentre(s.camp.x);
    const float cy = siegeCentre(s.camp.y);
    for (const Entity m : able) {
        for (const KnownEnemy& k : reg.get<Fighter>(m).known) {
            if (k.level == s.camp.level && siegeDist(k.x, k.y, cx, cy) <= rules_.campDefend && things_.spawned(k.pawn) && !health_.incapacitated(k.pawn)) {
                attacked = true;
            }
        }
    }
    const SiegeBuild* next = nullptr;
    for (const SiegeBuild& b : s.builds) {
        if (!b.done) {
            next = &b;
            break;
        }
    }
    std::vector<Entity> builders;
    if (!marching && !attacked && next != nullptr) {
        const int spare = static_cast<int>(able.size() - crews.size());
        const int wanted = std::max(1, static_cast<int>(std::lround(static_cast<float>(spare) * rules_.builderShare)));
        for (const Entity m : able) {
            if (static_cast<int>(builders.size()) < wanted && !in(crews, m)) {
                builders.push_back(m);
            }
        }
    }

    for (const Entity m : able) {
        Fighter& f = reg.get<Fighter>(m);
        const CellPos at = cellOf(reg.get<Position>(m));
        // On the march it walks to the camp without stopping to fight on the way.
        FightMode mode = marching ? FightMode::Seize : FightMode::Fight;
        CellPos goal = s.camp;
        const auto crewAt = std::find(crews.begin(), crews.end(), m);
        if (crewAt != crews.end()) {
            mode = FightMode::Seize;
            const Entity gun = guns[static_cast<size_t>(crewAt - crews.begin())];
            if (!construction_->workCell(reg.get<Footprint>(gun).rect(), at, goal)) {
                goal = s.camp;
            }
        } else if (in(builders, m)) {
            mode = FightMode::Seize;
            if (!construction_->workCell(buildRect(*next), at, goal)) {
                goal = s.camp;
            }
        }
        const bool changed = f.mode != mode || !f.hasGoal || f.goal != goal;
        f.advances = false;
        f.retreat = false;
        f.anchor = s.camp;
        f.leash = marching ? 0.0f : rules_.campLeash;
        f.hasGoal = true;
        f.goal = goal;
        if (changed) {
            f.mode = mode;
            if (mode == FightMode::Seize) {
                f.hasSpot = false;  // go to the new goal, not the old spot near it
            }
            fighters_.replan(m);
        }
    }
}

// Every second before the assault: the squad arrives at its camp and its first shells drop in, builders work, and
// crews load their mortars from the shells lying in the camp.
void RaidSystem::stepSiege(Squad& squad) {
    entt::registry& reg = things_.registry();
    Siege& s = squad.siege;
    const float cx = siegeCentre(s.camp.x);
    const float cy = siegeCentre(s.camp.y);
    if (s.stage == SiegeStage::March) {
        bool arrived = false;
        for (const Entity m : squad.members) {
            if (things_.spawned(m) && !health_.incapacitated(m)) {
                const Position& p = reg.get<Position>(m);
                arrived = arrived || (p.level == s.camp.level && siegeDist(p.x, p.y, cx, cy) <= rules_.campArrive);
            }
        }
        if (!arrived) {
            return;
        }
        s.stage = SiegeStage::Build;
        int mortars = 0;
        for (const SiegeBuild& b : s.builds) {
            mortars += b.def == rules_.siegeMortar ? 1 : 0;
        }
        siegeEvent(squad, "camp");
        dropShells(squad, rules_.shellsPerMortar * mortars);
        squad.nextThink = now();
        return;
    }
    // Builders (Seize mode: thinkSiege sent them) within reach of something unbuilt work on it.
    for (SiegeBuild& b : s.builds) {
        if (b.done) {
            continue;
        }
        const CellRect r = buildRect(b);
        float speed = 0.0f;
        for (const Entity m : squad.members) {
            if (!things_.spawned(m) || health_.incapacitated(m)) {
                continue;
            }
            const Fighter* f = reg.try_get<Fighter>(m);
            const Position& p = reg.get<Position>(m);
            if (f != nullptr && f->mode == FightMode::Seize && p.level == r.level && siegeDistToRect(p.x, p.y, r) <= rules_.buildReach) {
                speed += construction_->skillSpeed(m);
            }
        }
        if (speed <= 0.0f) {
            continue;
        }
        b.workLeft -= speed;
        if (b.workLeft <= 0.0f) {
            finishBuild(squad, b);
            squad.nextThink = now();
        }
    }
    // Crews at their guns load a shell when the gun is empty.
    if (fuel_ == nullptr) {
        return;
    }
    std::vector<Entity> stacks;
    bool looked = false;
    for (const SiegeBuild& b : s.builds) {
        if (b.built == kNoEntity || !things_.spawned(b.built) || !reg.all_of<Artillery>(b.built) || fuel_->roomFor(b.built) < 1 ||
            !artillery_->manned(b.built)) {
            continue;
        }
        const Entity crew = reg.get<Artillery>(b.built).crew;
        if (fighters_.factionOf(crew) == kPlayerFaction) {
            continue;
        }
        if (!looked) {
            siegeShells(squad, &stacks);
            looked = true;
        }
        while (!stacks.empty() && !things_.spawned(stacks.front())) {
            stacks.erase(stacks.begin());
        }
        if (stacks.empty()) {
            return;
        }
        const Entity stack = stacks.front();
        if (fuel_->add(b.built, 1) == 1) {
            if (reg.get<StackCount>(stack).count <= 1) {
                things_.destroy(stack);
                stacks.erase(stacks.begin());
            } else {
                things_.destroy(things_.splitOff(stack, 1));
            }
        }
    }
}

// Its work is done: built through Construction (pawns and items in the way are moved off), or given up if something
// else stands there now. The colony's haulers leave a raider gun's ammunition alone.
void RaidSystem::finishBuild(Squad& squad, SiegeBuild& build) {
    (void)squad;
    build.done = true;
    const DefId def = defs_.idOf("Things", build.def);
    const DefId stuff = build.stuff.empty() ? DefId{} : defs_.idOf("Things", build.stuff);
    std::string error;
    const Entity site = construction_->placeBlueprint(def, stuff, build.origin, build.rotation, error);
    if (site == kNoEntity) {
        return;
    }
    const Entity built = construction_->complete(site);
    if (built == kNoEntity) {
        return;
    }
    build.built = built;
    if (fuel_ != nullptr && things_.registry().all_of<Fuel>(built)) {
        fuel_->setRefuel(built, false, false, error);
    }
}

// Shells come down in stacks behind the camp (away from the colony), on open cells nothing is built on.
void RaidSystem::dropShells(Squad& squad, int count) {
    const DefId shell = defs_.idOf("Things", rules_.siegeShell);
    if (!shell.valid() || count <= 0) {
        return;
    }
    const Siege& s = squad.siege;
    const int limit = std::max(1, static_cast<int>(defs_.get(shell).integer("stackLimit", 25)));
    float ux = siegeCentre(squad.target.x) - siegeCentre(s.camp.x);
    float uy = siegeCentre(squad.target.y) - siegeCentre(s.camp.y);
    const float length = std::max(0.001f, std::sqrt(ux * ux + uy * uy));
    ux /= length;
    uy /= length;
    const CellPos behind{static_cast<int>(std::floor(siegeCentre(s.camp.x) - ux * 4.0f)),
                         static_cast<int>(std::floor(siegeCentre(s.camp.y) - uy * 4.0f)), s.camp.level};
    while (count > 0) {
        bool found = false;
        CellPos cell;
        for (int r = 0; r <= 6 && !found; ++r) {
            for (int dy = -r; dy <= r && !found; ++dy) {
                for (int dx = -r; dx <= r && !found; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dy)) != r) {
                        continue;
                    }
                    const CellPos c{behind.x + dx, behind.y + dy, behind.level};
                    if (!map_.onGround(c) || !map_.walkable(c) || map_.buildingAt(c) != kNoEntity || !things_.itemsAt(c).empty()) {
                        continue;
                    }
                    bool planned = false;
                    for (const SiegeBuild& b : s.builds) {
                        planned = planned || siegeInRect(buildRect(b), c);
                    }
                    if (!planned && siegeDist(siegeCentre(c.x), siegeCentre(c.y), siegeCentre(s.camp.x), siegeCentre(s.camp.y)) <= rules_.campSupplies) {
                        cell = c;
                        found = true;
                    }
                }
            }
        }
        if (!found) {
            return;
        }
        const int n = std::min(count, limit);
        const Entity stack = things_.spawn(shell, cell, n);
        if (stack == kNoEntity) {
            return;
        }
        count -= n;
        if (events_ != nullptr) {
            Event e;
            e.type = EventType::DropPodLanded;
            e.thing = stack;
            e.def = shell;
            e.cell = cell;
            e.squad = squad.id;
            e.count = n;
            events_->push_back(std::move(e));
        }
    }
}

// The camp's shells: those lying within campSupplies of its middle (their stacks in id order) and those in its guns.
int RaidSystem::siegeShells(const Squad& squad, std::vector<Entity>* stacks) const {
    const entt::registry& reg = things_.registry();
    const Siege& s = squad.siege;
    const DefId shell = defs_.idOf("Things", rules_.siegeShell);
    const float cx = siegeCentre(s.camp.x);
    const float cy = siegeCentre(s.camp.y);
    int total = 0;
    for (const auto [e, thing, pos, stack] : reg.view<const Thing, const Position, const StackCount>().each()) {
        if (thing.def != shell || pos.level != s.camp.level || siegeDist(pos.x, pos.y, cx, cy) > rules_.campSupplies) {
            continue;
        }
        total += stack.count;
        if (stacks != nullptr) {
            stacks->push_back(e);
        }
    }
    if (stacks != nullptr) {
        std::sort(stacks->begin(), stacks->end());
    }
    for (const SiegeBuild& b : s.builds) {
        if (b.built != kNoEntity && reg.valid(b.built)) {
            if (const Fuel* fuel = reg.try_get<Fuel>(b.built)) {
                total += static_cast<int>(std::floor(fuel->amount + 1.0e-4f));
            }
        }
    }
    return total;
}

// A manned siege mortar fires at the colony's pawns by itself (ArtillerySystem picks the nearest in range). With
// none in reach of its shells it shells the colony's buildings, the one nearest the colony's middle it can reach.
void RaidSystem::siegeAim(const Squad& squad, Entity mortar) {
    const entt::registry& reg = things_.registry();
    const Artillery& a = reg.get<Artillery>(mortar);
    std::string error;
    if (artillery_->enemyInRange(mortar) != kNoEntity) {
        if (a.hasAim) {
            artillery_->clearAim(mortar, error);
        }
        return;
    }
    std::vector<std::pair<float, Entity>> near;
    for (const auto [e, thing, pos, footprint] : reg.view<const Thing, const Position, const Footprint>().each()) {
        if (thing.category != ThingCategory::Building || pos.level != squad.target.level || reg.all_of<ConstructionSite>(e)) {
            continue;
        }
        bool ours = false;
        for (const Squad& other : state_.squads) {
            for (const SiegeBuild& b : other.siege.builds) {
                ours = ours || b.built == e;
            }
        }
        if (!ours) {
            near.emplace_back(siegeDist(pos.x, pos.y, siegeCentre(squad.target.x), siegeCentre(squad.target.y)), e);
        }
    }
    const size_t tries = std::min<size_t>(near.size(), 12);
    std::partial_sort(near.begin(), near.begin() + static_cast<std::ptrdiff_t>(tries), near.end());
    for (size_t i = 0; i < tries; ++i) {
        const CellRect r = reg.get<Footprint>(near[i].second).rect();
        const float x = static_cast<float>(r.minX) + static_cast<float>(r.sizeX) * 0.5f;
        const float y = static_cast<float>(r.minY) + static_cast<float>(r.sizeY) * 0.5f;
        if (a.hasAim && a.aimPawn == kNoEntity && a.aimX == x && a.aimY == y) {
            return;  // shelling it already
        }
        if (artillery_->aim(mortar, kNoEntity, x, y, error)) {
            return;
        }
    }
}

// The siege is over: the crews leave their guns and the squad goes for the colony as an assault, with the give-up
// clock started now.
void RaidSystem::startAssault(Squad& squad, const char* why) {
    entt::registry& reg = things_.registry();
    squad.siege.stage = SiegeStage::Assault;
    for (const SiegeBuild& b : squad.siege.builds) {
        if (b.built != kNoEntity && reg.valid(b.built) && reg.all_of<Artillery>(b.built)) {
            const Entity crew = reg.get<Artillery>(b.built).crew;
            if (crew != kNoEntity && std::find(squad.members.begin(), squad.members.end(), crew) != squad.members.end()) {
                artillery_->setForeignCrew(b.built, kNoEntity);
            }
        }
    }
    const float hourSeconds = static_cast<float>(kSecondsPerHour);
    squad.giveUpAt = now() + static_cast<int64_t>((rules_.giveUpHoursMin + rng_.unit() * (rules_.giveUpHoursMax - rules_.giveUpHoursMin)) * hourSeconds);
    for (const Entity m : squad.members) {
        Fighter* f = reg.valid(m) ? reg.try_get<Fighter>(m) : nullptr;
        if (f == nullptr || !things_.spawned(m)) {
            continue;
        }
        f->mode = FightMode::Fight;
        f->leash = 0.0f;
        f->advances = true;
        f->hasGoal = squad.hasTarget;
        f->goal = squad.target;
        fighters_.replan(m);
    }
    squad.nextThink = now();
    siegeEvent(squad, std::string("assault: ") + why);
}

void RaidSystem::siegeEvent(const Squad& squad, std::string message) {
    if (events_ == nullptr) {
        return;
    }
    Event e;
    e.type = EventType::SiegeChanged;
    e.squad = squad.id;
    e.cell = squad.siege.camp;
    e.count = static_cast<int>(squad.members.size());
    e.message = std::move(message);
    events_->push_back(std::move(e));
}

}  // namespace colony
