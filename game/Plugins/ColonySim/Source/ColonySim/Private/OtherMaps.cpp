// Other maps (W5, docs/design/other-maps.md): a caravan on another world tile walks onto that tile's
// map, a sim of its own made the first time and kept, stepped in time with the colony's. Colonists
// move between the sims with all their parts (moveOffMapThings, Save.cpp); the colony keeps an
// `AwayPawn` entity for each one away.
#include "ColonySim/OtherMaps.h"

#include "ColonySim/Caravans.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Random.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"
#include "ColonySim/World.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace colony {

namespace {

// The colony's placeholders, in id order (a view's order depends on history; saves and hashes must not).
std::vector<Entity> otherMapsAway(const entt::registry& reg) {
    std::vector<Entity> away(reg.view<const AwayPawn>().begin(), reg.view<const AwayPawn>().end());
    std::sort(away.begin(), away.end());
    return away;
}

}  // namespace

OtherMapRules OtherMapRules::read(const DefDatabase& defs) {
    OtherMapRules r;
    const Def* rules = defs.find("Rules", "OtherMapRules");
    if (rules == nullptr) {
        return r;
    }
    r.mapSize = std::clamp(static_cast<int>(rules->integer("mapSize", r.mapSize)), 32, 1024);
    r.settlementMapSize = std::clamp(static_cast<int>(rules->integer("settlementMapSize", r.settlementMapSize)), 32, 1024);
    return r;
}

const Sim* Sim::otherMap(int tile) const {
    for (const OtherMap& m : otherMaps_) {
        if (m.tile == tile) {
            return m.sim.get();
        }
    }
    return nullptr;
}

Sim* Sim::otherMap(int tile) {
    return const_cast<Sim*>(std::as_const(*this).otherMap(tile));
}

std::string Sim::placeName(int tile) const {
    if (world_ == nullptr || tile < 0 || tile >= world_->tileCount()) {
        return std::string();
    }
    const WorldTile& t = world_->tile(tile);
    if (t.settlement != kNoSettlement) {
        return world_->settlements()[static_cast<size_t>(t.settlement)].name;
    }
    const std::string& biome = world_->biomeName(t);
    const Def* def = defs_->find("Biomes", biome);
    return def != nullptr ? def->label() : biome;
}

bool Sim::visitMap(int caravanId, std::string& error) {
    if (world_ == nullptr || settings_.world.visit) {
        error = "there is no world map to visit";
        return false;
    }
    const Caravan* found = caravans_->find(caravanId);
    if (found == nullptr) {
        error = "no such caravan";
        return false;
    }
    if (found->state == CaravanState::Forming) {
        error = "the caravan hasn't left the map yet";
        return false;
    }
    if (found->state == CaravanState::Travelling) {
        error = "the caravan is on its way: stop it first";
        return false;
    }
    const int tile = found->tile;
    if (tile == world_->startTile()) {
        error = "the caravan is at the colony: use Enter";
        return false;
    }
    if (!world_->canVisit(tile)) {
        error = "there is no ground to walk on there";
        return false;
    }
    if (found->members.empty()) {
        error = "the caravan has nobody in it";
        return false;
    }
    OtherMap* map = nullptr;
    for (OtherMap& m : otherMaps_) {
        if (m.tile == tile) {
            map = &m;
        }
    }
    if (map == nullptr) {
        // The tile's own map: its site's seed, biome and climate, at the colony's time of year and day.
        const OtherMapRules rules = OtherMapRules::read(*defs_);
        SimSettings s;
        s.seed = splitMix64(settings_.seed ^ 0x6F746865726D6170ULL ^ (static_cast<uint64_t>(tile) << 24));
        s.mapSize = world_->tile(tile).settlement != kNoSettlement ? rules.settlementMapSize : rules.mapSize;
        s.pathBudget = settings_.pathBudget;
        s.checkpointEvery = 0;  // the colony's checkpoints take in its hash
        s.wildAnimals = settings_.wildAnimals;
        s.world = settings_.world;
        s.world.startTile = tile;
        s.world.visit = true;
        s.startSecond = time_.seconds;
        auto sim = std::make_unique<Sim>(s, defs_);
        if (!sim->setupErrors().empty() || sim->site() == nullptr) {
            error = "the map of that tile can't be made" + (sim->setupErrors().empty() ? std::string() : ": " + sim->setupErrors().front());
            return false;
        }
        otherMaps_.push_back(OtherMap{tile, time_.seconds, std::move(sim)});
        map = &otherMaps_.back();
        Event made;
        made.type = EventType::OtherMapMade;
        made.count = s.mapSize;
        made.amount = static_cast<float>(tile);
        made.message = placeName(tile);
        events_.push_back(std::move(made));
    }
    Caravan taken;
    caravans_->release(caravanId, taken);
    std::vector<Entity> roots;
    for (const CaravanWalker& w : taken.members) {
        roots.push_back(w.pawn);
    }
    const size_t pawns = roots.size();
    roots.insert(roots.end(), taken.items.begin(), taken.items.end());
    std::vector<Entity> made;
    if (!moveOffMapThings(*this, roots, *map->sim, std::vector<Entity>(roots.size(), kNoEntity), true, made, error)) {
        caravans_->adopt(std::move(taken), true);
        return false;
    }
    entt::registry& reg = things_->registry();
    for (size_t i = 0; i < pawns; ++i) {
        AwayPawn& away = reg.get<AwayPawn>(roots[i]);
        away.tile = tile;
        away.there = made[i];
    }
    // Over there it is that map's caravan, waiting on its tile, and it walks on from the side it came from.
    Caravan there = taken;
    for (size_t i = 0; i < pawns; ++i) {
        there.members[i].pawn = made[i];
    }
    there.items.assign(made.begin() + static_cast<std::ptrdiff_t>(pawns), made.end());
    there.state = CaravanState::Waiting;
    there.destination = -1;
    there.path.clear();
    there.progress = 0.0;
    there.cost = 0.0;
    const int arriving = map->sim->caravans_->adopt(std::move(there));
    std::string entering;
    map->sim->caravans_->enter(arriving, entering);
    Event entered;
    entered.type = EventType::OtherMapEntered;
    entered.count = caravanId;
    entered.amount = static_cast<float>(tile);
    entered.message = placeName(tile);
    events_.push_back(std::move(entered));
    return true;
}

bool Sim::formOnMap(int tile, const std::vector<Entity>& pawns, const std::vector<Entity>& items, int destination, int& formed,
                    std::string& error) {
    Sim* map = otherMap(tile);
    if (map == nullptr) {
        error = "no caravan has been to that tile";
        return false;
    }
    return map->caravans_->form(pawns, items, destination, formed, error);
}

void Sim::stepOtherMaps() {
    for (OtherMap& m : otherMaps_) {
        m.sim->stepOnce(false);
        std::vector<int> departed;
        for (const Event& e : m.sim->events()) {
            if (e.type == EventType::CaravanDeparted) {
                departed.push_back(e.count);
            }
        }
        for (const int id : departed) {
            takeCaravanFrom(m, id);
        }
    }
    if (otherMaps_.empty() || time_.seconds % kSecondsPerHour != 0) {
        return;
    }
    // A colonist who died away (or is gone from there) no longer has a place kept here.
    entt::registry& reg = things_->registry();
    for (const Entity e : otherMapsAway(reg)) {
        const AwayPawn& away = reg.get<AwayPawn>(e);
        const Sim* map = otherMap(away.tile);
        const bool gone = map == nullptr || !map->things().exists(away.there) ||
                          !map->things().registry().all_of<Thing>(away.there) ||
                          (map->things().registry().all_of<Health>(away.there) && map->things().registry().get<Health>(away.there).dead);
        if (gone) {
            reg.destroy(e);
        }
    }
}

void Sim::takeCaravanFrom(OtherMap& map, int caravanId) {
    Caravan taken;
    if (!map.sim->caravans_->release(caravanId, taken)) {
        return;
    }
    std::vector<Entity> roots;
    for (const CaravanWalker& w : taken.members) {
        roots.push_back(w.pawn);
    }
    const size_t pawns = roots.size();
    roots.insert(roots.end(), taken.items.begin(), taken.items.end());
    // Colonists from here fill the places kept for them.
    entt::registry& reg = things_->registry();
    std::vector<Entity> into(roots.size(), kNoEntity);
    for (const Entity e : otherMapsAway(reg)) {
        const AwayPawn& away = reg.get<AwayPawn>(e);
        for (size_t i = 0; i < pawns; ++i) {
            if (away.tile == map.tile && away.there == roots[i]) {
                into[i] = e;
            }
        }
    }
    std::vector<Entity> made;
    std::string error;
    if (!moveOffMapThings(*map.sim, roots, *this, into, false, made, error)) {
        map.sim->caravans_->adopt(std::move(taken), true);
        return;
    }
    Caravan here = taken;
    for (size_t i = 0; i < pawns; ++i) {
        here.members[i].pawn = made[i];
    }
    here.items.assign(made.begin() + static_cast<std::ptrdiff_t>(pawns), made.end());
    const int id = caravans_->adopt(std::move(here));
    Event left;
    left.type = EventType::OtherMapLeft;
    left.count = id;
    left.amount = static_cast<float>(map.tile);
    left.message = placeName(map.tile);
    events_.push_back(std::move(left));
}

void Sim::hashOtherMaps(StateHasher& h) const {
    h.add(static_cast<uint64_t>(otherMaps_.size()));
    for (const OtherMap& m : otherMaps_) {
        h.add(m.tile);
        h.add(m.madeAt);
        h.add(m.sim->stateHash());
    }
    const entt::registry& reg = things_->registry();
    for (const Entity e : otherMapsAway(reg)) {
        const AwayPawn& away = reg.get<AwayPawn>(e);
        h.add(static_cast<uint64_t>(entt::to_integral(e)));
        h.add(away.tile);
        h.add(static_cast<uint64_t>(entt::to_integral(away.there)));
        h.add(static_cast<uint64_t>(away.social.memories.size()));
        h.add(static_cast<uint64_t>(away.social.bonds.size()));
    }
}

void Sim::writeOtherMaps(std::vector<OtherMapView>& out) const {
    out.clear();
    const entt::registry& reg = things_->registry();
    for (const OtherMap& m : otherMaps_) {
        OtherMapView v;
        v.tile = m.tile;
        v.name = placeName(m.tile);
        v.size = m.sim->map().width();
        for (const auto [e, away] : reg.view<const AwayPawn>().each()) {
            (void)e;
            v.colonists += away.tile == m.tile ? 1 : 0;
        }
        v.caravans = static_cast<int>(m.sim->caravans().caravans().size());
        out.push_back(std::move(v));
    }
}

}  // namespace colony
