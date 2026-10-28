// Transport pods (docs/design/transport-pods.md): loading, launching and flying. Landing is the
// sim's (PodArrivals.cpp), since it may need another map or the caravans.
#include "ColonySim/TransportPods.h"

#include "ColonySim/Caravans.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Fuel.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/MentalBreaks.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Prisoners.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Things.h"
#include "ColonySim/World.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace colony {

namespace {

constexpr float kPodBoardReach = 1.5f;  // metres from the cell beside the pod that count as there

// The four cells along each edge of a rect (no corners): what "touching along an edge" means.
bool podRectsTouch(const CellRect& a, const CellRect& b) {
    if (a.level != b.level) {
        return false;
    }
    const bool overlapX = a.minX < b.minX + b.sizeX && b.minX < a.minX + a.sizeX;
    const bool overlapY = a.minY < b.minY + b.sizeY && b.minY < a.minY + a.sizeY;
    const bool besideX = a.minX + a.sizeX == b.minX || b.minX + b.sizeX == a.minX;
    const bool besideY = a.minY + a.sizeY == b.minY || b.minY + b.sizeY == a.minY;
    return (overlapY && besideX) || (overlapX && besideY);
}

}  // namespace

const char* podGroupStateName(PodGroupState state) {
    switch (state) {
        case PodGroupState::Loading: return "loading";
        case PodGroupState::Boarding: return "boarding";
        case PodGroupState::Ready: return "ready";
    }
    return "?";
}

const char* podArrivalName(PodArrival arrival) {
    switch (arrival) {
        case PodArrival::Land: return "Land";
        case PodArrival::Caravan: return "Caravan";
        case PodArrival::Gift: return "Gift";
    }
    return "?";
}

bool podArrivalFromName(const std::string& name, PodArrival& out) {
    for (const PodArrival a : {PodArrival::Land, PodArrival::Caravan, PodArrival::Gift}) {
        if (name == podArrivalName(a)) {
            out = a;
            return true;
        }
    }
    return false;
}

TransportPodRules TransportPodRules::read(const DefDatabase& defs) {
    TransportPodRules r;
    const Def* rules = defs.find("Rules", "TransportPodRules");
    if (rules == nullptr) {
        return r;
    }
    r.fuelPerTile = std::max(0.0, rules->number("fuelPerTile", r.fuelPerTile));
    r.minFuel = std::max(0.0, rules->number("minFuel", r.minFuel));
    r.ticksPerTile = std::max(0.0, rules->number("ticksPerTile", r.ticksPerTile));
    r.minFlightTicks = std::max(1.0, rules->number("minFlightTicks", r.minFlightTicks));
    r.maxRepaths = std::max(0, static_cast<int>(rules->integer("maxRepaths", r.maxRepaths)));
    return r;
}

TransportPodSystem::TransportPodSystem(Things& things, Map& map, Pathfinder& pathfinder, HealthSystem& health, FuelSystem& fuel,
                                       const DefDatabase& defs, const GameTime& time, const World* world)
    : things_(things),
      map_(map),
      pathfinder_(pathfinder),
      health_(health),
      fuel_(fuel),
      defs_(defs),
      time_(time),
      world_(world),
      rules_(TransportPodRules::read(defs)) {}

const PodGroup* TransportPodSystem::find(int id) const {
    for (const PodGroup& g : groups_) {
        if (g.id == id) {
            return &g;
        }
    }
    return nullptr;
}

PodGroup* TransportPodSystem::findMut(int id) {
    return const_cast<PodGroup*>(std::as_const(*this).find(id));
}

// ---- What things are -----------------------------------------------------------------------------

bool TransportPodSystem::isPod(Entity thing) const {
    const entt::registry& reg = things_.registry();
    return reg.valid(thing) && reg.all_of<Thing, Footprint>(thing) && !reg.all_of<ConstructionSite>(thing) &&
           defs_.get(reg.get<Thing>(thing).def).has("transportPod");
}

bool TransportPodSystem::isLauncher(Entity thing) const {
    const entt::registry& reg = things_.registry();
    return reg.valid(thing) && reg.all_of<Thing, Footprint, Fuel>(thing) && !reg.all_of<ConstructionSite>(thing) &&
           defs_.get(reg.get<Thing>(thing).def).flag("podLauncher");
}

double TransportPodSystem::massOfThing(Entity thing) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(thing) || !reg.all_of<Thing>(thing)) {
        return 0.0;
    }
    const Def& def = defs_.get(reg.get<Thing>(thing).def);
    const StackCount* stack = reg.try_get<StackCount>(thing);
    return def.number("stats.mass", 0.0) * static_cast<double>(stack != nullptr ? stack->count : 1);
}

double TransportPodSystem::massOf(const PodGroup& g) const {
    double mass = 0.0;
    for (const Entity e : g.wanted) {
        mass += massOfThing(e);
    }
    for (const Entity e : g.loaded) {
        mass += massOfThing(e);
    }
    for (const PodWalker& w : g.passengers) {
        mass += massOfThing(w.pawn);
    }
    return mass;
}

double TransportPodSystem::capacityOf(const PodGroup& g) const {
    double capacity = 0.0;
    const entt::registry& reg = things_.registry();
    for (const Entity pod : g.pods) {
        if (reg.valid(pod) && reg.all_of<Thing>(pod)) {
            capacity += defs_.get(reg.get<Thing>(pod).def).number("transportPod.massCapacity", 0.0);
        }
    }
    return capacity;
}

int TransportPodSystem::groupOfPod(Entity pod) const {
    for (const PodGroup& g : groups_) {
        if (std::find(g.pods.begin(), g.pods.end(), pod) != g.pods.end()) {
            return g.id;
        }
    }
    return 0;
}

bool TransportPodSystem::hasLoad(const PodGroup& g) const {
    return !g.loaded.empty() || hasPassengersAboard(g);
}

bool TransportPodSystem::hasPassengersAboard(const PodGroup& g) const {
    return std::any_of(g.passengers.begin(), g.passengers.end(), [](const PodWalker& w) { return w.walk == 3; });
}

int TransportPodSystem::distance(int from, int to) const {
    if (world_ == nullptr || from < 0 || to < 0 || from == to) {
        return 0;
    }
    int dx = std::abs(world_->tileX(to) - world_->tileX(from));
    dx = std::min(dx, world_->width() - dx);
    const int dy = std::abs(world_->tileY(to) - world_->tileY(from));
    return std::max(dx, dy);
}

Entity TransportPodSystem::launcherOf(Entity pod) const {
    const entt::registry& reg = things_.registry();
    if (!isPod(pod) || !things_.spawned(pod)) {
        return kNoEntity;
    }
    // Every pod on the map and every launcher, in id order: the first pods take the first launchers they touch.
    std::vector<Entity> pods;
    std::vector<Entity> launchers;
    for (const auto [e, thing, footprint] : reg.view<const Thing, const Footprint>().each()) {
        (void)thing;
        (void)footprint;
        if (!things_.spawned(e)) {
            continue;
        }
        if (isPod(e)) {
            pods.push_back(e);
        } else if (isLauncher(e)) {
            launchers.push_back(e);
        }
    }
    std::sort(pods.begin(), pods.end());
    std::sort(launchers.begin(), launchers.end());
    std::vector<uint8_t> taken(launchers.size(), 0);
    for (const Entity p : pods) {
        const CellRect rect = reg.get<Footprint>(p).rect();
        for (size_t i = 0; i < launchers.size(); ++i) {
            if (taken[i] == 0 && podRectsTouch(rect, reg.get<Footprint>(launchers[i]).rect())) {
                taken[i] = 1;
                if (p == pod) {
                    return launchers[i];
                }
                break;
            }
        }
        if (p == pod) {
            return kNoEntity;
        }
    }
    return kNoEntity;
}

int TransportPodSystem::rangeOf(const PodGroup& g) const {
    if (g.pods.empty()) {
        return -1;
    }
    const entt::registry& reg = things_.registry();
    double least = -1.0;
    for (const Entity pod : g.pods) {
        const Entity launcher = launcherOf(pod);
        if (launcher == kNoEntity) {
            return -1;
        }
        const double fuel = static_cast<double>(reg.get<Fuel>(launcher).amount);
        least = least < 0.0 ? fuel : std::min(least, fuel);
    }
    if (least < rules_.minFuel) {
        return -1;
    }
    return rules_.fuelPerTile > 0.0 ? static_cast<int>(std::floor(least / rules_.fuelPerTile + 1e-9)) : 1000000;
}

// ---- Commands -----------------------------------------------------------------------------------

bool TransportPodSystem::canBoard(Entity pawn, std::string& error) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Colonist, Thing>(pawn)) {
        error = "only colonists can ride in a transport pod";
        return false;
    }
    if (!things_.spawned(pawn)) {
        error = "that colonist is not on the map";
        return false;
    }
    if (reg.all_of<CaravanMember>(pawn)) {
        error = "that colonist is in a caravan";
        return false;
    }
    if (health_.incapacitated(pawn)) {
        error = "that colonist can't walk";
        return false;
    }
    if (reg.all_of<Prisoner>(pawn)) {
        error = "a prisoner can't ride in a transport pod";
        return false;
    }
    if (reg.all_of<PlayerControl>(pawn)) {
        error = "let go of the colonist you walk first";
        return false;
    }
    if (const Fighter* fighter = reg.try_get<Fighter>(pawn); fighter != nullptr) {
        error = fighter->drafted ? "undraft that colonist first" : "that colonist is fighting";
        return false;
    }
    if (const MentalBreaker* b = reg.try_get<MentalBreaker>(pawn); b != nullptr && b->active) {
        error = "that colonist is in a mental break";
        return false;
    }
    return true;
}

bool TransportPodSystem::load(const std::vector<Entity>& pods, const std::vector<Entity>& pawns, const std::vector<Entity>& items,
                              int& id, std::string& error) {
    if (pods.empty()) {
        error = "name at least one transport pod";
        return false;
    }
    if (pawns.empty() && items.empty()) {
        error = "nothing to load";
        return false;
    }
    entt::registry& reg = things_.registry();
    PodGroup g;
    g.id = nextGroup_;
    for (const Entity pod : pods) {
        if (!isPod(pod) || !things_.spawned(pod)) {
            error = "that is not a finished transport pod on the map";
            return false;
        }
        if (groupOfPod(pod) != 0 || std::find(g.pods.begin(), g.pods.end(), pod) != g.pods.end()) {
            error = "a pod is loading already, or named twice";
            return false;
        }
        g.pods.push_back(pod);
    }
    const auto named = [&](Entity e) {
        for (const PodGroup& other : groups_) {
            if (std::find(other.wanted.begin(), other.wanted.end(), e) != other.wanted.end() ||
                std::any_of(other.passengers.begin(), other.passengers.end(), [&](const PodWalker& w) { return w.pawn == e; })) {
                return true;
            }
        }
        return std::find(g.wanted.begin(), g.wanted.end(), e) != g.wanted.end() ||
               std::any_of(g.passengers.begin(), g.passengers.end(), [&](const PodWalker& w) { return w.pawn == e; });
    };
    for (const Entity pawn : pawns) {
        if (!canBoard(pawn, error)) {
            return false;
        }
        if (named(pawn)) {
            error = "a colonist is going in pods already, or named twice";
            return false;
        }
        PodWalker w;
        w.pawn = pawn;
        g.passengers.push_back(w);
    }
    for (const Entity item : items) {
        if (!reg.valid(item) || !reg.all_of<Thing, StackCount>(item) || reg.get<Thing>(item).category != ThingCategory::Item) {
            error = "only items can be loaded";
            return false;
        }
        if (!things_.spawned(item)) {
            error = "an item to load is not lying on the map";
            return false;
        }
        if (named(item)) {
            error = "an item is going in pods already, or named twice";
            return false;
        }
        g.wanted.push_back(item);
    }
    const double mass = massOf(g);
    const double capacity = capacityOf(g);
    if (mass > capacity + 1e-6) {
        error = "too heavy: " + std::to_string(static_cast<int>(std::ceil(mass))) + " kg for " +
                std::to_string(static_cast<int>(capacity)) + " kg the pods can carry";
        return false;
    }
    g.spot = cellOf(reg.get<Position>(g.pods.front()));
    id = g.id;
    ++nextGroup_;
    groups_.push_back(std::move(g));
    emit(EventType::PodsLoading, id, kNoEntity, "", static_cast<float>(groups_.back().pods.size()), groups_.back().spot);
    return true;
}

bool TransportPodSystem::cancel(int id, std::string& error) {
    PodGroup* g = findMut(id);
    if (g == nullptr) {
        error = "no such pod group";
        return false;
    }
    CellPos at = g->spot;
    for (const Entity pod : g->pods) {
        if (things_.spawned(pod)) {
            at = cellOf(things_.registry().get<Position>(pod));
            break;
        }
    }
    spill(*g, at);
    emit(EventType::PodsCancelled, id, kNoEntity, "cancelled");
    groups_.erase(std::remove_if(groups_.begin(), groups_.end(), [&](const PodGroup& k) { return k.id == id; }), groups_.end());
    return true;
}

bool TransportPodSystem::launch(int id, int tile, PodArrival arrival, CellPos cell, std::string& error) {
    PodGroup* g = findMut(id);
    if (g == nullptr) {
        error = "no such pod group";
        return false;
    }
    if (!hasLoad(*g)) {
        error = "nothing is in the pods yet";
        return false;
    }
    const int from = world_ != nullptr ? world_->startTile() : -1;
    const int to = tile < 0 ? from : tile;
    const int tiles = distance(from, to);
    const double need = std::max(rules_.minFuel, rules_.fuelPerTile * static_cast<double>(tiles));
    entt::registry& reg = things_.registry();
    std::vector<Entity> launchers;
    for (const Entity pod : g->pods) {
        const Entity launcher = launcherOf(pod);
        if (launcher == kNoEntity) {
            error = "a pod has no pod launcher beside it";
            return false;
        }
        if (static_cast<double>(reg.get<Fuel>(launcher).amount) + 1e-6 < need) {
            error = "out of range: " + std::to_string(tiles) + " tiles takes " + std::to_string(static_cast<int>(std::ceil(need))) +
                    " chemfuel in each launcher";
            return false;
        }
        launchers.push_back(launcher);
    }
    PodFlight f;
    f.id = nextFlight_++;
    f.pods = static_cast<int>(g->pods.size());
    f.from = from;
    f.to = to;
    f.arrival = arrival;
    f.cell = cell;
    f.seconds = rimWorldTicksToSeconds(std::max(rules_.minFlightTicks, rules_.ticksPerTile * static_cast<double>(tiles)));
    for (PodWalker& w : g->passengers) {
        if (w.walk == 3) {
            f.pawns.push_back(w.pawn);
            continue;
        }
        if (w.walk == 1) {
            pathfinder_.cancel(w.path);
        }
        if (reg.valid(w.pawn)) {
            reg.remove<PodPassenger>(w.pawn);
            if (Mover* mover = reg.try_get<Mover>(w.pawn)) {
                mover->path.clear();
                mover->next = 0;
                mover->gaveUp = false;
            }
        }
    }
    f.items = g->loaded;
    const std::vector<Entity> pods = g->pods;
    groups_.erase(std::remove_if(groups_.begin(), groups_.end(), [&](const PodGroup& k) { return k.id == id; }), groups_.end());
    for (const Entity launcher : launchers) {
        fuel_.use(launcher, static_cast<float>(need));
    }
    for (const Entity pod : pods) {
        if (jobs_ != nullptr) {
            jobs_->forget(pod);
        }
        things_.destroy(pod);
    }
    emit(EventType::PodsLaunched, f.id, kNoEntity, podArrivalName(arrival), static_cast<float>(to));
    flights_.push_back(std::move(f));
    return true;
}

// ---- Loading ------------------------------------------------------------------------------------

Entity TransportPodSystem::podWanting(Entity stack, CellPos near) const {
    const entt::registry& reg = things_.registry();
    for (const PodGroup& g : groups_) {
        if (g.state != PodGroupState::Loading || std::find(g.wanted.begin(), g.wanted.end(), stack) == g.wanted.end()) {
            continue;
        }
        Entity best = kNoEntity;
        int bestDist = 0;
        for (const Entity pod : g.pods) {
            if (!things_.spawned(pod)) {
                continue;
            }
            const CellPos c = cellOf(reg.get<Position>(pod));
            const int d = std::abs(c.x - near.x) + std::abs(c.y - near.y);
            if (best == kNoEntity || d < bestDist) {
                best = pod;
                bestDist = d;
            }
        }
        return best;
    }
    return kNoEntity;
}

bool TransportPodSystem::putIn(Entity pod, Entity carried) {
    const int id = groupOfPod(pod);
    PodGroup* g = findMut(id);
    if (g == nullptr || things_.spawned(carried)) {
        return false;
    }
    g->wanted.erase(std::remove(g->wanted.begin(), g->wanted.end(), carried), g->wanted.end());
    if (std::find(g->loaded.begin(), g->loaded.end(), carried) == g->loaded.end()) {
        g->loaded.push_back(carried);
    }
    emit(EventType::PodLoaded, id, carried, "", 0.0f, cellOf(things_.registry().get<Position>(pod)));
    return true;
}

// ---- Steps --------------------------------------------------------------------------------------

void TransportPodSystem::step() {
    std::vector<int> ids;
    for (const PodGroup& g : groups_) {
        ids.push_back(g.id);
    }
    for (const int id : ids) {
        if (PodGroup* g = findMut(id)) {
            stepGroup(*g);
        }
    }
    for (PodFlight& f : flights_) {
        f.progress += 1.0;
    }
    const auto landed = std::stable_partition(flights_.begin(), flights_.end(), [](const PodFlight& f) { return f.progress < f.seconds; });
    arrived_.insert(arrived_.end(), std::make_move_iterator(landed), std::make_move_iterator(flights_.end()));
    flights_.erase(landed, flights_.end());
}

std::vector<PodFlight> TransportPodSystem::takeArrived() {
    std::vector<PodFlight> out = std::move(arrived_);
    arrived_.clear();
    return out;
}

void TransportPodSystem::stepGroup(PodGroup& g) {
    entt::registry& reg = things_.registry();
    // Pods taken down by other means (deconstructed) are gone from the group; with none left, the load spills.
    g.pods.erase(std::remove_if(g.pods.begin(), g.pods.end(), [&](Entity pod) { return !isPod(pod) || !things_.spawned(pod); }),
                 g.pods.end());
    if (g.pods.empty()) {
        const int id = g.id;
        spill(g, g.spot);
        emit(EventType::PodsCancelled, id, kNoEntity, "the pods are gone");
        groups_.erase(std::remove_if(groups_.begin(), groups_.end(), [&](const PodGroup& k) { return k.id == id; }), groups_.end());
        return;
    }
    g.wanted.erase(std::remove_if(g.wanted.begin(), g.wanted.end(), [&](Entity e) { return !reg.valid(e) || !reg.all_of<Thing>(e); }),
                   g.wanted.end());
    if (g.state == PodGroupState::Loading) {
        if (!g.wanted.empty()) {
            return;
        }
        g.state = PodGroupState::Boarding;
        callPassengers(g);
    }
    if (g.state != PodGroupState::Boarding) {
        return;
    }
    std::vector<std::pair<Entity, std::string>> dropped;
    for (PodWalker& w : g.passengers) {
        if (w.walk == 3) {
            continue;
        }
        const Entity pawn = w.pawn;
        std::string why;
        if (health_.incapacitated(pawn)) {
            why = "can't walk";
        } else if (reg.all_of<Fighter>(pawn)) {
            why = "went to fight";
        } else if (const MentalBreaker* b = reg.try_get<MentalBreaker>(pawn); b != nullptr && b->active) {
            why = "mental break";
        } else if (reg.all_of<PlayerControl>(pawn)) {
            why = "taken over by the player";
        } else if (!things_.spawned(pawn)) {
            why = "not on the map";
        }
        if (!why.empty()) {
            dropped.emplace_back(pawn, why);
            continue;
        }
        Mover& mover = reg.get<Mover>(pawn);
        const Position& pos = reg.get<Position>(pawn);
        const float dx = pos.x - (static_cast<float>(w.to.x) + 0.5f);
        const float dy = pos.y - (static_cast<float>(w.to.y) + 0.5f);
        if (dx * dx + dy * dy <= kPodBoardReach * kPodBoardReach) {
            mover.path.clear();
            mover.next = 0;
            mover.gaveUp = false;
            mover.vx = 0.0f;
            mover.vy = 0.0f;
            if (w.walk == 1) {
                pathfinder_.cancel(w.path);
                w.path = PathTicket{};
            }
            things_.despawn(pawn);
            reg.remove<PodPassenger>(pawn);
            w.walk = 3;
            emit(EventType::PodLoaded, g.id, pawn, "", 0.0f, w.to);
            continue;
        }
        if (w.walk == 0) {
            w.path = pathfinder_.request(cellOf(pos), w.to);
            w.walk = 1;
        } else if (w.walk == 1) {
            if (pathfinder_.status(w.path) == PathStatus::Queued) {
                continue;
            }
            std::vector<CellPos> route;
            const bool found = pathfinder_.takePath(w.path, route);
            w.path = PathTicket{};
            if (!found) {
                dropped.emplace_back(pawn, "can't reach the pods");
                continue;
            }
            smoothPath(map_, pos.x, pos.y, route);
            mover.walkTo(pos.x, pos.y, std::move(route));
            w.walk = 2;
        } else if (w.walk == 2 && (mover.gaveUp || mover.arrived())) {
            mover.gaveUp = false;
            if (w.repaths >= rules_.maxRepaths) {
                dropped.emplace_back(pawn, "can't reach the pods");
                continue;
            }
            ++w.repaths;
            w.path = pathfinder_.request(cellOf(pos), w.to);
            w.walk = 1;
        }
    }
    for (const auto& [pawn, why] : dropped) {
        dropPassenger(g, pawn, why);
    }
    if (std::all_of(g.passengers.begin(), g.passengers.end(), [](const PodWalker& w) { return w.walk == 3; })) {
        g.state = PodGroupState::Ready;
        emit(EventType::PodsReady, g.id, kNoEntity, "", static_cast<float>(g.passengers.size()), g.spot);
    }
}

void TransportPodSystem::callPassengers(PodGroup& g) {
    entt::registry& reg = things_.registry();
    std::vector<std::pair<Entity, std::string>> dropped;
    for (PodWalker& w : g.passengers) {
        std::string why;
        if (!canBoard(w.pawn, why)) {
            dropped.emplace_back(w.pawn, why);
            continue;
        }
        w.to = besidePod(g, cellOf(reg.get<Position>(w.pawn)));
        if (w.to.x < 0) {
            dropped.emplace_back(w.pawn, "can't reach the pods");
            continue;
        }
        if (jobs_ != nullptr) {
            jobs_->interrupt(w.pawn, "getting into a transport pod");
        }
        reg.emplace_or_replace<PodPassenger>(w.pawn, PodPassenger{g.id});
        w.walk = 0;
    }
    for (const auto& [pawn, why] : dropped) {
        dropPassenger(g, pawn, why);
    }
}

// A walkable cell beside one of the group's pods that `from` can reach: the nearest pod's first such cell.
CellPos TransportPodSystem::besidePod(const PodGroup& g, CellPos from) const {
    const entt::registry& reg = things_.registry();
    std::vector<std::pair<int, Entity>> byDistance;
    for (const Entity pod : g.pods) {
        if (!things_.spawned(pod)) {
            continue;
        }
        const CellPos c = cellOf(reg.get<Position>(pod));
        byDistance.emplace_back(std::abs(c.x - from.x) + std::abs(c.y - from.y), pod);
    }
    std::sort(byDistance.begin(), byDistance.end());
    Regions& regions = map_.regions();
    for (const auto& [d, pod] : byDistance) {
        (void)d;
        const CellRect r = reg.get<Footprint>(pod).rect();
        for (int y = r.minY - 1; y <= r.minY + r.sizeY; ++y) {
            for (int x = r.minX - 1; x <= r.minX + r.sizeX; ++x) {
                const bool edge = (x == r.minX - 1 || x == r.minX + r.sizeX) != (y == r.minY - 1 || y == r.minY + r.sizeY);
                const CellPos cell{x, y, r.level};
                if (edge && map_.inBounds(cell) && map_.walkable(cell) && regions.canReach(from, cell)) {
                    return cell;
                }
            }
        }
    }
    return CellPos{-1, -1, 0};
}

void TransportPodSystem::dropPassenger(PodGroup& g, Entity pawn, const std::string& why) {
    entt::registry& reg = things_.registry();
    g.passengers.erase(std::remove_if(g.passengers.begin(), g.passengers.end(),
                                      [&](const PodWalker& w) {
                                          if (w.pawn == pawn && w.walk == 1) {
                                              pathfinder_.cancel(w.path);
                                          }
                                          return w.pawn == pawn;
                                      }),
                       g.passengers.end());
    if (reg.valid(pawn)) {
        reg.remove<PodPassenger>(pawn);
        if (Mover* mover = reg.try_get<Mover>(pawn)) {
            mover->path.clear();
            mover->next = 0;
            mover->gaveUp = false;
        }
    }
    emit(EventType::PodPassengerDropped, g.id, pawn, why);
}

// Everything in the group comes out beside `at`; passengers on the way stop where they are.
void TransportPodSystem::spill(PodGroup& g, CellPos at) {
    entt::registry& reg = things_.registry();
    std::vector<Entity> pawns;
    for (const PodWalker& w : g.passengers) {
        if (w.walk == 1) {
            pathfinder_.cancel(w.path);
        }
        if (!reg.valid(w.pawn)) {
            continue;
        }
        reg.remove<PodPassenger>(w.pawn);
        if (Mover* mover = reg.try_get<Mover>(w.pawn)) {
            mover->path.clear();
            mover->next = 0;
            mover->gaveUp = false;
        }
        if (w.walk == 3) {
            pawns.push_back(w.pawn);
        }
    }
    landHere(pawns, g.loaded, at);
    g.passengers.clear();
    g.loaded.clear();
    g.wanted.clear();
}

CellPos TransportPodSystem::landHere(const std::vector<Entity>& pawns, const std::vector<Entity>& items, CellPos cell) {
    entt::registry& reg = things_.registry();
    CellPos at = cell;
    if (at.x < 0 || !map_.inBounds(at)) {
        at = CellPos{map_.width() / 2, map_.height() / 2, 0};
    }
    at.level = 0;
    // The nearest walkable cell (pods come down on open ground).
    bool found = map_.walkable(at);
    for (int ring = 1; ring < 32 && !found; ++ring) {
        for (int y = at.y - ring; y <= at.y + ring && !found; ++y) {
            for (int x = at.x - ring; x <= at.x + ring && !found; ++x) {
                const CellPos c{x, y, 0};
                if (std::max(std::abs(x - at.x), std::abs(y - at.y)) == ring && map_.inBounds(c) && map_.walkable(c)) {
                    at = c;
                    found = true;
                }
            }
        }
    }
    for (const Entity pawn : pawns) {
        if (!reg.valid(pawn)) {
            continue;
        }
        reg.remove<PodPassenger>(pawn);
        if (Mover* mover = reg.try_get<Mover>(pawn)) {
            mover->path.clear();
            mover->next = 0;
            mover->gaveUp = false;
            mover->vx = 0.0f;
            mover->vy = 0.0f;
        }
        things_.spawnAt(pawn, at);
    }
    size_t next = 0;
    for (int ring = 0; ring < 12 && next < items.size(); ++ring) {
        for (int y = at.y - ring; y <= at.y + ring && next < items.size(); ++y) {
            for (int x = at.x - ring; x <= at.x + ring && next < items.size(); ++x) {
                const CellPos c{x, y, 0};
                if (std::max(std::abs(x - at.x), std::abs(y - at.y)) != ring || !map_.inBounds(c) || !map_.walkable(c) ||
                    !things_.itemsAt(c).empty()) {
                    continue;
                }
                if (!reg.valid(items[next]) || things_.spawnAt(items[next], c)) {
                    ++next;
                }
            }
        }
    }
    for (; next < items.size(); ++next) {
        if (reg.valid(items[next])) {
            things_.spawnAt(items[next], at);
        }
    }
    return at;
}

void TransportPodSystem::forget(Entity thing) {
    const entt::registry& reg = things_.registry();
    const int podGroup = groupOfPod(thing);
    if (podGroup != 0) {
        PodGroup* g = findMut(podGroup);
        g->pods.erase(std::remove(g->pods.begin(), g->pods.end(), thing), g->pods.end());
        if (g->pods.empty()) {
            const CellPos at = things_.spawned(thing) ? cellOf(reg.get<Position>(thing)) : g->spot;
            spill(*g, at);
            emit(EventType::PodsCancelled, podGroup, kNoEntity, "the pods are gone");
            groups_.erase(std::remove_if(groups_.begin(), groups_.end(), [&](const PodGroup& k) { return k.id == podGroup; }),
                          groups_.end());
        }
    }
    for (PodGroup& g : groups_) {
        g.wanted.erase(std::remove(g.wanted.begin(), g.wanted.end(), thing), g.wanted.end());
        g.loaded.erase(std::remove(g.loaded.begin(), g.loaded.end(), thing), g.loaded.end());
        if (std::any_of(g.passengers.begin(), g.passengers.end(), [&](const PodWalker& w) { return w.pawn == thing; })) {
            dropPassenger(g, thing, "gone");
        }
    }
    for (std::vector<PodFlight>* list : {&flights_, &arrived_}) {
        for (PodFlight& f : *list) {
            f.pawns.erase(std::remove(f.pawns.begin(), f.pawns.end(), thing), f.pawns.end());
            f.items.erase(std::remove(f.items.begin(), f.items.end(), thing), f.items.end());
        }
    }
}

void TransportPodSystem::emit(EventType type, int count, Entity thing, const std::string& message, float amount, CellPos cell) const {
    if (events_ == nullptr) {
        return;
    }
    Event e;
    e.type = type;
    e.thing = thing;
    e.count = count;
    e.message = message;
    e.amount = amount;
    e.cell = cell;
    events_->push_back(std::move(e));
}

// ---- Saves --------------------------------------------------------------------------------------

bool TransportPodSystem::restore(std::vector<PodGroup> groups, std::vector<PodFlight> flights, int nextGroup, int nextFlight,
                                 std::string& error) {
    entt::registry& reg = things_.registry();
    for (const PodGroup& g : groups) {
        if (g.id <= 0 || g.id >= nextGroup) {
            error = "the save has a pod group with a bad id";
            return false;
        }
        for (const Entity pod : g.pods) {
            if (!reg.valid(pod) || !reg.all_of<Thing>(pod)) {
                error = "the save has a pod group with a pod that doesn't exist";
                return false;
            }
        }
        for (const PodWalker& w : g.passengers) {
            if (!reg.valid(w.pawn) || !reg.all_of<Thing>(w.pawn) || (w.walk == 3) == things_.spawned(w.pawn)) {
                error = "the save has a pod passenger that isn't where it should be";
                return false;
            }
        }
        for (const Entity item : g.loaded) {
            if (!reg.valid(item) || !reg.all_of<Thing>(item) || things_.spawned(item)) {
                error = "the save has a loaded item that isn't in its pod";
                return false;
            }
        }
        for (const Entity item : g.wanted) {
            if (!reg.valid(item) || !reg.all_of<Thing>(item)) {
                error = "the save has an item to load that doesn't exist";
                return false;
            }
        }
    }
    for (const PodFlight& f : flights) {
        if (f.id <= 0 || f.id >= nextFlight) {
            error = "the save has a pod flight with a bad id";
            return false;
        }
        for (const std::vector<Entity>* list : {&f.pawns, &f.items}) {
            for (const Entity e : *list) {
                if (!reg.valid(e) || !reg.all_of<Thing>(e) || things_.spawned(e)) {
                    error = "the save has a pod flight with something that isn't in the air";
                    return false;
                }
            }
        }
    }
    groups_ = std::move(groups);
    flights_ = std::move(flights);
    arrived_.clear();
    nextGroup_ = nextGroup;
    nextFlight_ = nextFlight;
    for (const PodGroup& g : groups_) {
        for (const PodWalker& w : g.passengers) {
            if (g.state == PodGroupState::Boarding && w.walk != 3) {
                reg.emplace_or_replace<PodPassenger>(w.pawn, PodPassenger{g.id});
            }
        }
    }
    return true;
}

void TransportPodSystem::hashState(StateHasher& h) const {
    h.add(nextGroup_);
    h.add(nextFlight_);
    h.add(static_cast<uint64_t>(groups_.size()));
    for (const PodGroup& g : groups_) {
        h.add(g.id);
        h.add(static_cast<int>(g.state));
        for (const std::vector<Entity>* list : {&g.pods, &g.wanted, &g.loaded}) {
            h.add(static_cast<uint64_t>(list->size()));
            for (const Entity e : *list) {
                h.add(static_cast<uint64_t>(entt::to_integral(e)));
            }
        }
        for (const PodWalker& w : g.passengers) {
            h.add(static_cast<uint64_t>(entt::to_integral(w.pawn)));
            h.add(static_cast<int>(w.walk));
            h.add(static_cast<int>(w.repaths));
            h.add(w.to.x);
            h.add(w.to.y);
        }
        h.add(g.spot.x);
        h.add(g.spot.y);
    }
    h.add(static_cast<uint64_t>(flights_.size()));
    for (const PodFlight& f : flights_) {
        h.add(f.id);
        for (const std::vector<Entity>* list : {&f.pawns, &f.items}) {
            h.add(static_cast<uint64_t>(list->size()));
            for (const Entity e : *list) {
                h.add(static_cast<uint64_t>(entt::to_integral(e)));
            }
        }
        h.add(f.pods);
        h.add(f.from);
        h.add(f.to);
        h.add(static_cast<int>(f.arrival));
        h.add(f.cell.x);
        h.add(f.cell.y);
        h.add(f.progress);
        h.add(f.seconds);
    }
}

}  // namespace colony
