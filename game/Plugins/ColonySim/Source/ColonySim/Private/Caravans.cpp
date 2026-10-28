#include "ColonySim/Caravans.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/MentalBreaks.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Prisoners.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Things.h"
#include "ColonySim/World.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <utility>

namespace colony {

namespace {

constexpr int kCaravanMaxRepaths = 5;
constexpr float kCaravanExitReach = 1.5f;  // metres from the exit cell's centre that count as there

// Food first eaten, as RimWorld's preferability: a better meal before a worse one, raw food last.
int caravanFoodRank(const std::string& preferability) {
    if (preferability == "MealLavish") return 7;
    if (preferability == "MealFine") return 6;
    if (preferability == "MealSimple") return 5;
    if (preferability == "MealAwful") return 4;
    if (preferability == "RawTasty") return 3;
    if (preferability == "RawBad") return 2;
    return 1;
}

uint64_t caravanEdgeKey(int a, int b) {
    const auto lo = static_cast<uint64_t>(std::min(a, b));
    const auto hi = static_cast<uint64_t>(std::max(a, b));
    return (lo << 32) | hi;
}

}  // namespace

const char* caravanStateName(CaravanState state) {
    switch (state) {
        case CaravanState::Forming: return "forming";
        case CaravanState::Travelling: return "travelling";
        case CaravanState::Resting: return "resting";
        case CaravanState::Waiting: return "waiting";
    }
    return "?";
}

CaravanRules CaravanRules::read(const DefDatabase& defs) {
    CaravanRules r;
    const Def* rules = defs.find("Rules", "CaravanRules");
    if (rules == nullptr) {
        return r;
    }
    r.ticksPerTile = std::max(1.0, rules->number("ticksPerTile", r.ticksPerTile));
    r.zeroMassSpeedFactor = rules->number("zeroMassSpeedFactor", r.zeroMassSpeedFactor);
    r.fullMassSpeedFactor = rules->number("fullMassSpeedFactor", r.fullMassSpeedFactor);
    r.carryPerBodySize = rules->number("carryPerBodySize", r.carryPerBodySize);
    r.roadFactor = rules->number("roadFactor", r.roadFactor);
    for (size_t i = 0; i < r.hillOffsets.size() && i < rules->count("hillOffsets"); ++i) {
        r.hillOffsets[i] = rules->numberAt("hillOffsets", i, r.hillOffsets[i]);
    }
    r.winterFrom = rules->number("winterFrom", r.winterFrom);
    r.winterTo = rules->number("winterTo", r.winterTo);
    r.winterOffset = rules->number("winterOffset", r.winterOffset);
    r.restFromHour = static_cast<int>(rules->integer("restFromHour", r.restFromHour));
    r.restToHour = static_cast<int>(rules->integer("restToHour", r.restToHour));
    r.diagonalFactor = rules->number("diagonalFactor", r.diagonalFactor);
    r.formHours = rules->number("formHours", r.formHours);
    r.exitDepth = std::max(0, static_cast<int>(rules->integer("exitDepth", r.exitDepth)));
    return r;
}

CaravanSystem::CaravanSystem(Things& things, Map& map, Pathfinder& pathfinder, HealthSystem& health, NeedsSystem& needs,
                             const DefDatabase& defs, const GameTime& time, const World* world)
    : things_(things),
      map_(map),
      pathfinder_(pathfinder),
      health_(health),
      needs_(needs),
      defs_(defs),
      time_(time),
      world_(world),
      rules_(CaravanRules::read(defs)) {
    nutrition_.assign(defs.size(), 0.0f);
    foodRank_.assign(defs.size(), 0);
    for (const DefId id : defs.idsOfKind("Things")) {
        const Def& def = defs.get(id);
        const float nutrition = static_cast<float>(def.number("stats.nutrition", 0.0));
        // Caravans don't eat people (nobody packs them as food) or anything that has no nutrition.
        if (nutrition > 0.0f && !def.flag("humanlike") && def.text("category") != "Pawn") {
            nutrition_[id.value] = nutrition;
            foodRank_[id.value] = caravanFoodRank(def.text("ingestible.preferability"));
        }
    }
    if (world_ != nullptr) {
        for (const std::string& name : world_->biomeNames()) {
            const Def* biome = defs.find("Biomes", name);
            biomeDifficulty_.push_back(biome != nullptr ? biome->number("movementDifficulty", 1.0) : 1.0);
            std::array<double, 4> shift{0.0, 0.0, 0.0, 0.0};
            if (biome != nullptr && biome->has("climate.seasonMeanTemperature")) {
                static constexpr const char* kSeasons[4] = {"spring", "summer", "fall", "winter"};
                double mean = 0.0;
                for (int s = 0; s < 4; ++s) {
                    shift[static_cast<size_t>(s)] =
                        biome->number((std::string("climate.seasonMeanTemperature.") + kSeasons[s]).c_str(), 0.0);
                    mean += shift[static_cast<size_t>(s)] * 0.25;
                }
                for (double& v : shift) {
                    v -= mean;
                }
            }
            biomeSeasonShift_.push_back(shift);
        }
        for (const WorldRoad& road : world_->roads()) {
            roads_.push_back(caravanEdgeKey(road.tileA, road.tileB));
        }
        std::sort(roads_.begin(), roads_.end());
    }
}

const Caravan* CaravanSystem::find(int id) const {
    for (const Caravan& c : caravans_) {
        if (c.id == id) {
            return &c;
        }
    }
    return nullptr;
}

Caravan* CaravanSystem::findMut(int id) {
    for (Caravan& c : caravans_) {
        if (c.id == id) {
            return &c;
        }
    }
    return nullptr;
}

// ---- The world as a caravan sees it ----------------------------------------------------------

double CaravanSystem::difficulty(int tile, int season) const {
    if (world_ == nullptr || tile < 0 || tile >= world_->tileCount()) {
        return 0.0;
    }
    const WorldTile& t = world_->tile(tile);
    if (t.water || t.biome == kNoBiome || t.hilliness == Hilliness::Impassable || t.biome >= biomeDifficulty_.size()) {
        return 0.0;
    }
    double d = biomeDifficulty_[t.biome] + rules_.hillOffsets[static_cast<size_t>(t.hilliness)];
    // The season's mean temperature on the tile: the biome's season shape around the tile's own yearly mean (W2).
    const double temperature = static_cast<double>(t.temperature) + biomeSeasonShift_[t.biome][static_cast<size_t>(season & 3)];
    if (temperature < rules_.winterFrom && rules_.winterFrom > rules_.winterTo) {
        d += rules_.winterOffset * std::min(1.0, (rules_.winterFrom - temperature) / (rules_.winterFrom - rules_.winterTo));
    }
    return d;
}

double CaravanSystem::stepSeconds(int from, int to, int season) const {
    if (world_ == nullptr) {
        return -1.0;
    }
    int dir = -1;
    for (int d = 0; d < 8; ++d) {
        if (world_->neighbour(from, d) == to) {
            dir = d;
            break;
        }
    }
    const double d = difficulty(to, season);
    if (dir < 0 || d <= 0.0) {
        return -1.0;
    }
    double seconds = rimWorldTicksToSeconds(rules_.ticksPerTile) * d;
    if (dir % 2 == 1) {
        seconds *= rules_.diagonalFactor;
    }
    if (std::binary_search(roads_.begin(), roads_.end(), caravanEdgeKey(from, to))) {
        seconds *= rules_.roadFactor;
    }
    return seconds;
}

std::vector<int> CaravanSystem::findPath(int from, int to, int season) const {
    std::vector<int> path;
    if (world_ == nullptr || from == to || from < 0 || to < 0 || from >= world_->tileCount() || to >= world_->tileCount() ||
        difficulty(to, season) <= 0.0) {
        return path;
    }
    // Dijkstra over the tiles; ties go to the lower tile index, so the path is the same everywhere.
    const auto n = static_cast<size_t>(world_->tileCount());
    std::vector<double> best(n, -1.0);
    std::vector<int> cameFrom(n, -1);
    using Entry = std::pair<double, int>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
    best[static_cast<size_t>(from)] = 0.0;
    open.push({0.0, from});
    while (!open.empty()) {
        const auto [cost, tile] = open.top();
        open.pop();
        if (cost > best[static_cast<size_t>(tile)]) {
            continue;
        }
        if (tile == to) {
            break;
        }
        for (int d = 0; d < 8; ++d) {
            const int next = world_->neighbour(tile, d);
            if (next < 0) {
                continue;
            }
            const double step = stepSeconds(tile, next, season);
            if (step < 0.0) {
                continue;
            }
            const double total = cost + step;
            double& known = best[static_cast<size_t>(next)];
            if (known < 0.0 || total < known) {
                known = total;
                cameFrom[static_cast<size_t>(next)] = tile;
                open.push({total, next});
            }
        }
    }
    if (best[static_cast<size_t>(to)] < 0.0) {
        return path;
    }
    for (int t = to; t != from; t = cameFrom[static_cast<size_t>(t)]) {
        path.push_back(t);
    }
    std::reverse(path.begin(), path.end());
    return path;
}

double CaravanSystem::massOfItem(Entity item) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(item) || !reg.all_of<Thing>(item)) {
        return 0.0;
    }
    const Def& def = defs_.get(reg.get<Thing>(item).def);
    const StackCount* stack = reg.try_get<StackCount>(item);
    return def.number("stats.mass", 0.0) * static_cast<double>(stack != nullptr ? stack->count : 1);
}

double CaravanSystem::capacityOfPawn(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Thing>(pawn) || health_.incapacitated(pawn)) {
        return 0.0;
    }
    const Def& def = defs_.get(reg.get<Thing>(pawn).def);
    // RimWorld's caravan capacity (not the hauling `carryingCapacity`): 35 kg per body size.
    return def.number("race.baseBodySize", 1.0) * rules_.carryPerBodySize;
}

double CaravanSystem::massOf(const Caravan& c) const {
    double mass = 0.0;
    for (const Entity item : c.items) {
        mass += massOfItem(item);
    }
    // A downed member is carried.
    for (const CaravanWalker& w : c.members) {
        if (health_.incapacitated(w.pawn)) {
            mass += defs_.get(things_.registry().get<Thing>(w.pawn).def).number("stats.mass", 0.0);
        }
    }
    return mass;
}

double CaravanSystem::capacityOf(const Caravan& c) const {
    double capacity = 0.0;
    for (const CaravanWalker& w : c.members) {
        capacity += capacityOfPawn(w.pawn);
    }
    return capacity;
}

double CaravanSystem::speedFactor(const Caravan& c) const {
    const double capacity = capacityOf(c);
    const double mass = massOf(c);
    if (capacity <= 0.0 || mass > capacity) {
        return 0.0;
    }
    const double load = mass / capacity;
    return rules_.zeroMassSpeedFactor + (rules_.fullMassSpeedFactor - rules_.zeroMassSpeedFactor) * load;
}

bool CaravanSystem::restHour() const {
    const int h = time_.hourOfDay();
    return rules_.restFromHour > rules_.restToHour ? (h >= rules_.restFromHour || h < rules_.restToHour)
                                                   : (h >= rules_.restFromHour && h < rules_.restToHour);
}

bool CaravanSystem::resting(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const CaravanMember* member = reg.valid(pawn) ? reg.try_get<CaravanMember>(pawn) : nullptr;
    if (member == nullptr || !restHour()) {
        return false;
    }
    const Caravan* c = find(member->caravan);
    return c != nullptr && c->state != CaravanState::Forming;
}

// ---- Commands -----------------------------------------------------------------------------------

bool CaravanSystem::canJoin(Entity pawn, std::string& error) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Colonist, Thing>(pawn)) {
        error = "only colonists can go in a caravan";
        return false;
    }
    if (!things_.spawned(pawn)) {
        error = "that colonist is not on the map";
        return false;
    }
    if (reg.all_of<CaravanMember>(pawn)) {
        error = "that colonist is in a caravan already";
        return false;
    }
    if (health_.incapacitated(pawn)) {
        error = "that colonist can't walk";
        return false;
    }
    if (reg.all_of<Prisoner>(pawn)) {
        error = "a prisoner can't go in a caravan";
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

bool CaravanSystem::form(const std::vector<Entity>& pawns, const std::vector<Entity>& items, int destination, int& id,
                         std::string& error) {
    if (world_ == nullptr || world_->startTile() < 0) {
        error = "there is no world map to travel";
        return false;
    }
    if (pawns.empty()) {
        error = "a caravan needs at least one colonist";
        return false;
    }
    if (destination >= world_->tileCount()) {
        error = "there is no such tile";
        return false;
    }
    if (destination >= 0 && destination != world_->startTile() && findPath(world_->startTile(), destination, time_.season()).empty()) {
        error = "there is no way to that tile";
        return false;
    }
    entt::registry& reg = things_.registry();
    Caravan c;
    c.id = nextId_;
    c.tile = world_->startTile();
    c.destination = destination == world_->startTile() ? -1 : destination;
    c.formedAt = time_.seconds;
    for (const Entity pawn : pawns) {
        if (!canJoin(pawn, error)) {
            return false;
        }
        for (const CaravanWalker& w : c.members) {
            if (w.pawn == pawn) {
                error = "a colonist is named twice";
                return false;
            }
        }
        CaravanWalker w;
        w.pawn = pawn;
        c.members.push_back(w);
    }
    for (const Entity item : items) {
        if (!reg.valid(item) || !reg.all_of<Thing, StackCount>(item) || reg.get<Thing>(item).category != ThingCategory::Item) {
            error = "only items can be packed";
            return false;
        }
        if (!things_.spawned(item)) {
            error = "an item to pack is not lying on the map";
            return false;
        }
        if (std::find(c.items.begin(), c.items.end(), item) != c.items.end()) {
            error = "an item is named twice";
            return false;
        }
        c.items.push_back(item);
    }
    const double mass = massOf(c);
    const double capacity = capacityOf(c);
    if (mass > capacity) {
        error = "too heavy: " + std::to_string(static_cast<int>(std::ceil(mass))) + " kg for " +
                std::to_string(static_cast<int>(capacity)) + " kg the colonists can carry";
        return false;
    }
    // The exit: the map edge facing the way it goes, reachable from the first member.
    c.exit = exitCell(c, cellOf(reg.get<Position>(c.members.front().pawn)));
    if (c.exit.x < 0) {
        error = "there is no way to the edge of the map";
        return false;
    }
    for (const CaravanWalker& w : c.members) {
        if (jobs_ != nullptr) {
            jobs_->interrupt(w.pawn, "joined a caravan");
        }
        reg.emplace_or_replace<CaravanMember>(w.pawn, CaravanMember{c.id});
    }
    id = c.id;
    ++nextId_;
    caravans_.push_back(std::move(c));
    emit(EventType::CaravanFormed, caravans_.back(), kNoEntity, "", static_cast<float>(caravans_.back().members.size()));
    return true;
}

bool CaravanSystem::move(int id, int tile, std::string& error) {
    Caravan* c = findMut(id);
    if (c == nullptr) {
        error = "no such caravan";
        return false;
    }
    if (c->state == CaravanState::Forming) {
        error = "the caravan hasn't left the map yet";
        return false;
    }
    if (world_ == nullptr || tile < 0 || tile >= world_->tileCount()) {
        error = "there is no such tile";
        return false;
    }
    if (tile == c->tile) {
        c->path.clear();
        c->destination = tile;
        arrive(*c);
        return true;
    }
    std::vector<int> path = findPath(c->tile, tile, time_.season());
    if (path.empty()) {
        error = "there is no way to that tile";
        return false;
    }
    c->destination = tile;
    c->path = std::move(path);
    c->progress = 0.0;
    c->state = restHour() ? CaravanState::Resting : CaravanState::Travelling;
    startNextTile(*c);
    return true;
}

bool CaravanSystem::stop(int id, std::string& error) {
    Caravan* c = findMut(id);
    if (c == nullptr) {
        error = "no such caravan";
        return false;
    }
    if (c->state == CaravanState::Forming) {
        error = "the caravan hasn't left the map yet";
        return false;
    }
    c->state = CaravanState::Waiting;
    c->destination = -1;
    c->path.clear();
    c->progress = 0.0;
    c->cost = 0.0;
    return true;
}

bool CaravanSystem::enter(int id, std::string& error) {
    Caravan* c = findMut(id);
    if (c == nullptr) {
        error = "no such caravan";
        return false;
    }
    if (c->state == CaravanState::Forming) {
        error = "the caravan hasn't left the map yet";
        return false;
    }
    if (world_ == nullptr || c->tile != world_->startTile()) {
        error = "the caravan is not at the colony";
        return false;
    }
    enterMap(*c);
    return true;
}

// ---- Steps --------------------------------------------------------------------------------------

void CaravanSystem::step() {
    // A caravan that came home is gone from the list; iterate by id so that is safe.
    std::vector<int> ids;
    for (const Caravan& c : caravans_) {
        ids.push_back(c.id);
    }
    for (const int id : ids) {
        Caravan* c = findMut(id);
        if (c == nullptr) {
            continue;
        }
        if (c->state == CaravanState::Forming) {
            stepForming(*c);
        } else {
            stepWorld(*c);
        }
    }
}

void CaravanSystem::dropMember(Caravan& c, Entity pawn, const std::string& why) {
    entt::registry& reg = things_.registry();
    c.members.erase(std::remove_if(c.members.begin(), c.members.end(),
                                   [&](const CaravanWalker& w) {
                                       if (w.pawn == pawn && w.walk == 1) {
                                           pathfinder_.cancel(w.path);
                                       }
                                       return w.pawn == pawn;
                                   }),
                    c.members.end());
    if (reg.valid(pawn)) {
        reg.remove<CaravanMember>(pawn);
        if (Mover* mover = reg.try_get<Mover>(pawn)) {
            mover->path.clear();
            mover->next = 0;
            mover->gaveUp = false;
        }
    }
    emit(EventType::CaravanMemberLost, c, pawn, why);
}

void CaravanSystem::stepForming(Caravan& c) {
    entt::registry& reg = things_.registry();
    const bool late = static_cast<double>(time_.seconds - c.formedAt) >= rules_.formHours * static_cast<double>(kSecondsPerHour);
    std::vector<std::pair<Entity, std::string>> dropped;
    for (CaravanWalker& w : c.members) {
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
        } else if (late) {
            why = "left behind";
        }
        if (!why.empty()) {
            dropped.emplace_back(pawn, why);
            continue;
        }
        Mover& mover = reg.get<Mover>(pawn);
        const Position& pos = reg.get<Position>(pawn);
        const float dx = pos.x - (static_cast<float>(c.exit.x) + 0.5f);
        const float dy = pos.y - (static_cast<float>(c.exit.y) + 0.5f);
        if (dx * dx + dy * dy <= kCaravanExitReach * kCaravanExitReach) {
            leaveMap(c, w);
            continue;
        }
        if (w.walk == 0) {
            w.path = pathfinder_.request(cellOf(pos), c.exit);
            w.walk = 1;
        } else if (w.walk == 1) {
            if (pathfinder_.status(w.path) == PathStatus::Queued) {
                continue;
            }
            std::vector<CellPos> route;
            const bool found = pathfinder_.takePath(w.path, route);
            w.path = PathTicket{};
            if (!found) {
                dropped.emplace_back(pawn, "can't reach the edge");
                continue;
            }
            smoothPath(map_, pos.x, pos.y, route);
            mover.walkTo(pos.x, pos.y, std::move(route));
            w.walk = 2;
        } else if (w.walk == 2) {
            if (mover.gaveUp || mover.arrived()) {
                mover.gaveUp = false;
                if (w.repaths >= kCaravanMaxRepaths) {
                    dropped.emplace_back(pawn, "can't reach the edge");
                    continue;
                }
                ++w.repaths;
                w.path = pathfinder_.request(cellOf(pos), c.exit);
                w.walk = 1;
            }
        }
    }
    for (const auto& [pawn, why] : dropped) {
        dropMember(c, pawn, why);
    }
    if (c.members.empty()) {
        const int id = c.id;
        emit(EventType::CaravanCancelled, c, kNoEntity, "nobody left to go");
        caravans_.erase(std::remove_if(caravans_.begin(), caravans_.end(), [&](const Caravan& k) { return k.id == id; }),
                        caravans_.end());
        return;
    }
    const bool allGone = std::all_of(c.members.begin(), c.members.end(), [](const CaravanWalker& w) { return w.walk == 3; });
    if (allGone) {
        setOut(c);
    }
}

void CaravanSystem::leaveMap(Caravan& c, CaravanWalker& w) {
    entt::registry& reg = things_.registry();
    if (Mover* mover = reg.try_get<Mover>(w.pawn)) {
        mover->path.clear();
        mover->next = 0;
        mover->gaveUp = false;
        mover->vx = 0.0f;
        mover->vy = 0.0f;
    }
    if (w.walk == 1) {
        pathfinder_.cancel(w.path);
        w.path = PathTicket{};
    }
    things_.despawn(w.pawn);
    w.walk = 3;
    (void)c;
}

void CaravanSystem::setOut(Caravan& c) {
    entt::registry& reg = things_.registry();
    // The goods are packed: what still lies on the map goes along, what is gone is left out.
    std::vector<Entity> packed;
    for (const Entity item : c.items) {
        if (!things_.spawned(item)) {
            continue;
        }
        if (jobs_ != nullptr) {
            jobs_->forget(item);
        }
        things_.despawn(item);
        packed.push_back(item);
    }
    c.items = std::move(packed);
    (void)reg;
    c.state = CaravanState::Waiting;
    c.tile = world_->startTile();
    c.cameFrom = c.tile;
    emit(EventType::CaravanDeparted, c, kNoEntity, "", static_cast<float>(c.tile));
    if (c.destination >= 0) {
        std::string error;
        if (!move(c.id, c.destination, error)) {
            c.destination = -1;
        }
    }
}

void CaravanSystem::startNextTile(Caravan& c) {
    c.progress = 0.0;
    c.cost = 0.0;
    if (c.path.empty()) {
        return;
    }
    double cost = stepSeconds(c.tile, c.path.front(), time_.season());
    if (cost < 0.0) {
        // The way closed (can't happen on a world that doesn't change, but a plan must not walk into sea).
        std::vector<int> path = c.destination >= 0 ? findPath(c.tile, c.destination, time_.season()) : std::vector<int>{};
        if (path.empty()) {
            c.state = CaravanState::Waiting;
            c.destination = -1;
            c.path.clear();
            return;
        }
        c.path = std::move(path);
        cost = stepSeconds(c.tile, c.path.front(), time_.season());
    }
    c.cost = cost;
}

void CaravanSystem::stepWorld(Caravan& c) {
    entt::registry& reg = things_.registry();
    // A member who died on the way is carried along as a corpse.
    std::vector<Entity> dead;
    for (const CaravanWalker& w : c.members) {
        if (reg.all_of<Health>(w.pawn) && reg.get<Health>(w.pawn).dead) {
            dead.push_back(w.pawn);
        }
    }
    for (const Entity pawn : dead) {
        dropMember(c, pawn, "died");
        c.items.push_back(pawn);
    }
    const bool anyAlive = std::any_of(c.members.begin(), c.members.end(), [&](const CaravanWalker& w) {
        return !health_.incapacitated(w.pawn);
    });
    if (!anyAlive) {
        // Nobody is left who can walk: the caravan is lost, and all it carried with it.
        const int id = c.id;
        emit(EventType::CaravanLost, c, kNoEntity, c.members.empty() ? "all died" : "nobody can walk");
        std::vector<Entity> gone = c.items;
        for (const CaravanWalker& w : c.members) {
            gone.push_back(w.pawn);
            reg.remove<CaravanMember>(w.pawn);
        }
        caravans_.erase(std::remove_if(caravans_.begin(), caravans_.end(), [&](const Caravan& k) { return k.id == id; }),
                        caravans_.end());
        for (const Entity thing : gone) {
            if (reg.valid(thing)) {
                things_.destroy(thing);
            }
        }
        return;
    }
    feed(c);
    if (c.state == CaravanState::Waiting) {
        return;
    }
    c.state = restHour() ? CaravanState::Resting : CaravanState::Travelling;
    if (c.state != CaravanState::Travelling || c.path.empty() || c.cost <= 0.0) {
        return;
    }
    c.progress += speedFactor(c);  // one game second at the load's speed; 0 when overloaded
    if (c.progress < c.cost) {
        return;
    }
    c.cameFrom = c.tile;
    c.tile = c.path.front();
    c.path.erase(c.path.begin());
    if (c.path.empty()) {
        arrive(c);
    } else {
        startNextTile(c);
    }
}

void CaravanSystem::arrive(Caravan& c) {
    c.state = CaravanState::Waiting;
    c.path.clear();
    c.progress = 0.0;
    c.cost = 0.0;
    c.destination = -1;
    std::string settlement;
    if (world_ != nullptr) {
        const int s = world_->tile(c.tile).settlement;
        if (s != kNoSettlement) {
            settlement = world_->settlements()[static_cast<size_t>(s)].name;
        }
    }
    emit(EventType::CaravanArrived, c, kNoEntity, settlement, static_cast<float>(c.tile));
    if (world_ != nullptr && c.tile == world_->startTile()) {
        enterMap(c);
    }
}

void CaravanSystem::feed(Caravan& c) {
    entt::registry& reg = things_.registry();
    const float seekBelow = needs_.rules().foodSeekBelow;
    for (const CaravanWalker& w : c.members) {
        Needs* needs = reg.try_get<Needs>(w.pawn);
        if (needs == nullptr || needs->food >= seekBelow || health_.incapacitated(w.pawn)) {
            continue;
        }
        Entity food = kNoEntity;
        int bestRank = 0;
        for (const Entity item : c.items) {
            if (!reg.valid(item) || !reg.all_of<Thing, StackCount>(item)) {
                continue;
            }
            const DefId def = reg.get<Thing>(item).def;
            const int rank = def.value < foodRank_.size() ? foodRank_[def.value] : 0;
            if (rank > bestRank || (rank == bestRank && rank > 0 && item < food)) {
                bestRank = rank;
                food = item;
            }
        }
        if (food == kNoEntity) {
            continue;
        }
        const DefId def = reg.get<Thing>(food).def;
        const float nutrition = nutrition_[def.value];
        StackCount& stack = reg.get<StackCount>(food);
        const int wanted = std::max(1, static_cast<int>(std::ceil((1.0f - needs->food) / nutrition - 0.001f)));
        const int eaten = std::min(wanted, stack.count);
        const float before = needs->food;
        needs->food = std::min(1.0f, needs->food + nutrition * static_cast<float>(eaten));
        stack.count -= eaten;
        Event e;
        e.type = EventType::PawnAte;
        e.thing = w.pawn;
        e.def = def;
        e.amount = needs->food - before;
        e.message = "ground";  // no table on the road: RimWorld's "ate without table"
        e.count = c.id;
        if (events_ != nullptr) {
            events_->push_back(std::move(e));
        }
        if (stack.count <= 0) {
            c.items.erase(std::remove(c.items.begin(), c.items.end(), food), c.items.end());
            if (jobs_ != nullptr) {
                jobs_->forget(food);
            }
            things_.destroy(food);
        }
    }
}

// The map edge facing `toward` (a world direction from the start tile), with north at the map's
// row 0: the exit (or entry) is the walkable cell nearest the middle of that edge, `exitDepth`
// cells in, that `from` can reach.
CellPos CaravanSystem::exitCell(const Caravan& c, const CellPos& from) const {
    int side = 0;  // 0 north (y 0), 1 east, 2 south, 3 west
    const int target = c.destination >= 0 ? c.destination : -1;
    if (world_ != nullptr && target >= 0) {
        int dx = world_->tileX(target) - world_->tileX(c.tile);
        const int w = world_->width();
        if (dx > w / 2) dx -= w;
        if (dx < -w / 2) dx += w;
        const int dy = world_->tileY(target) - world_->tileY(c.tile);
        if (std::abs(dx) >= std::abs(dy)) {
            side = dx >= 0 ? 1 : 3;
        } else {
            side = dy >= 0 ? 2 : 0;
        }
    }
    const int w = map_.width();
    const int h = map_.height();
    const int d = std::min(rules_.exitDepth, std::min(w, h) / 2);
    Regions& regions = map_.regions();
    for (int attempt = 0; attempt < 4; ++attempt) {
        const int s = (side + attempt) % 4;
        const int length = s % 2 == 0 ? w : h;
        for (int k = 0; k < length; ++k) {
            // From the middle outward: 0, +1, -1, +2, -2...
            const int offset = (k + 1) / 2 * (k % 2 == 1 ? 1 : -1);
            const int along = length / 2 + offset;
            if (along < 0 || along >= length) {
                continue;
            }
            const CellPos cell = s == 0 ? CellPos{along, d, 0}
                               : s == 1 ? CellPos{w - 1 - d, along, 0}
                               : s == 2 ? CellPos{along, h - 1 - d, 0}
                                        : CellPos{d, along, 0};
            if (map_.walkable(cell) && regions.canReach(from, cell)) {
                return cell;
            }
        }
    }
    return CellPos{-1, -1, 0};
}

void CaravanSystem::enterMap(Caravan& c) {
    entt::registry& reg = things_.registry();
    // In at the edge facing the tile it came from; with no such tile, the north edge.
    Caravan facing = c;
    facing.destination = c.cameFrom != c.tile ? c.cameFrom : -1;
    CellPos centre{map_.width() / 2, map_.height() / 2, 0};
    // The middle of the map (or any walkable cell near it) is what the entry must reach.
    for (int r = 0; r < 64 && !map_.walkable(centre); ++r) {
        centre.x = std::min(map_.width() - 1, centre.x + 1);
    }
    const CellPos entry = exitCell(facing, centre);
    const CellPos at = entry.x >= 0 ? entry : centre;
    // Pawns on the entry cell, items on the free walkable cells round it.
    std::vector<Entity> pawns;
    for (const CaravanWalker& w : c.members) {
        pawns.push_back(w.pawn);
    }
    for (const Entity pawn : pawns) {
        reg.remove<CaravanMember>(pawn);
        things_.spawnAt(pawn, at);
    }
    size_t next = 0;
    for (int ring = 0; ring < 12 && next < c.items.size(); ++ring) {
        for (int y = at.y - ring; y <= at.y + ring && next < c.items.size(); ++y) {
            for (int x = at.x - ring; x <= at.x + ring && next < c.items.size(); ++x) {
                if (std::max(std::abs(x - at.x), std::abs(y - at.y)) != ring) {
                    continue;
                }
                const CellPos cell{x, y, 0};
                if (!map_.walkable(cell) || !things_.itemsAt(cell).empty()) {
                    continue;
                }
                if (things_.spawnAt(c.items[next], cell)) {
                    ++next;
                }
            }
        }
    }
    // Anything that found no room (a map with no ground near the edge) is put down where the pawns stand.
    for (; next < c.items.size(); ++next) {
        things_.spawnAt(c.items[next], at);
    }
    const int id = c.id;
    emit(EventType::CaravanEntered, c, kNoEntity, "", static_cast<float>(pawns.size()));
    if (events_ != nullptr && !events_->empty()) {
        events_->back().cell = at;
    }
    caravans_.erase(std::remove_if(caravans_.begin(), caravans_.end(), [&](const Caravan& k) { return k.id == id; }),
                    caravans_.end());
}

void CaravanSystem::forget(Entity thing) {
    entt::registry& reg = things_.registry();
    for (Caravan& c : caravans_) {
        c.items.erase(std::remove(c.items.begin(), c.items.end(), thing), c.items.end());
        const bool member = std::any_of(c.members.begin(), c.members.end(), [&](const CaravanWalker& w) { return w.pawn == thing; });
        if (member) {
            dropMember(c, thing, "gone");
        }
    }
    if (reg.valid(thing)) {
        reg.remove<CaravanMember>(thing);
    }
}

void CaravanSystem::emit(EventType type, const Caravan& c, Entity thing, const std::string& message, float amount) const {
    if (events_ == nullptr) {
        return;
    }
    Event e;
    e.type = type;
    e.thing = thing;
    e.count = c.id;
    e.cell = c.exit;
    e.message = message;
    e.amount = amount;
    events_->push_back(std::move(e));
}

// ---- Saves --------------------------------------------------------------------------------------

bool CaravanSystem::restore(std::vector<Caravan> caravans, int nextId, std::string& error) {
    entt::registry& reg = things_.registry();
    for (const Caravan& c : caravans) {
        if (c.id <= 0 || c.id >= nextId || (world_ != nullptr && (c.tile < 0 || c.tile >= world_->tileCount())) ||
            (world_ == nullptr && c.state != CaravanState::Forming)) {
            error = "the save has a caravan that doesn't fit the world";
            return false;
        }
        for (const CaravanWalker& w : c.members) {
            if (!reg.valid(w.pawn) || !reg.all_of<Thing>(w.pawn) || (w.walk == 3) == things_.spawned(w.pawn)) {
                error = "the save has a caravan member that isn't where its caravan is";
                return false;
            }
        }
        for (const Entity item : c.items) {
            const bool packed = c.state != CaravanState::Forming;
            if (!reg.valid(item) || !reg.all_of<Thing>(item) || (packed && things_.spawned(item))) {
                error = "the save has a caravan item that isn't where its caravan is";
                return false;
            }
        }
    }
    caravans_ = std::move(caravans);
    nextId_ = nextId;
    for (const Caravan& c : caravans_) {
        for (const CaravanWalker& w : c.members) {
            reg.emplace_or_replace<CaravanMember>(w.pawn, CaravanMember{c.id});
        }
    }
    return true;
}

void CaravanSystem::hashState(StateHasher& h) const {
    h.add(nextId_);
    h.add(static_cast<uint64_t>(caravans_.size()));
    for (const Caravan& c : caravans_) {
        h.add(c.id);
        h.add(static_cast<int>(c.state));
        for (const CaravanWalker& w : c.members) {
            h.add(static_cast<uint64_t>(entt::to_integral(w.pawn)));
            h.add(static_cast<int>(w.walk));
            h.add(static_cast<int>(w.repaths));
        }
        for (const Entity item : c.items) {
            h.add(static_cast<uint64_t>(entt::to_integral(item)));
        }
        h.add(c.tile);
        h.add(c.cameFrom);
        h.add(c.destination);
        for (const int t : c.path) {
            h.add(t);
        }
        h.add(c.progress);
        h.add(c.cost);
        h.add(c.exit.x);
        h.add(c.exit.y);
        h.add(c.formedAt);
    }
}

}  // namespace colony
