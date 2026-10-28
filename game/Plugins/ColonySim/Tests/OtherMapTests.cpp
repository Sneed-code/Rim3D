#include "ColonySim/Caravans.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Health.h"
#include "ColonySim/OtherMaps.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Social.h"
#include "ColonySim/Things.h"
#include "ColonySim/World.h"

#include <doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// W5: other maps (docs/design/other-maps.md).

#define OTHERMAP_REQUIRE(cond) \
    do {                       \
        CHECK((cond));         \
        if (!(cond)) {         \
            return;            \
        }                      \
    } while (false)

namespace {

std::shared_ptr<const DefDatabase> otherMapDefs() {
    static std::shared_ptr<const DefDatabase> db;
    if (db == nullptr) {
        auto loaded = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *loaded, errors);
        db = loaded;
    }
    return db;
}

SimSettings otherMapSettings() {
    SimSettings s;
    s.seed = 42;
    s.mapSize = 64;
    s.scenario = "Crashlanded";
    s.world.enabled = true;
    return s;
}

std::vector<Entity> otherMapPawns(Sim& sim) {
    std::vector<Entity> pawns;
    for (const auto [e, c, pos] : sim.things().registry().view<const Colonist, const Position>().each()) {
        (void)pos;
        if (!c.protagonist) {
            pawns.push_back(e);
        }
    }
    std::sort(pawns.begin(), pawns.end());
    pawns.resize(std::min<size_t>(pawns.size(), 2));
    return pawns;
}

Entity otherMapMeals(Sim& sim) {
    Entity best = kNoEntity;
    int most = 0;
    for (const auto [e, thing, pos, stack] : sim.things().registry().view<const Thing, const Position, const StackCount>().each()) {
        (void)pos;
        if (sim.defs().get(thing.def).defName() == "MealSurvivalPack" && stack.count > most) {
            most = stack.count;
            best = e;
        }
    }
    return best;
}

// A neighbouring land tile a caravan can reach.
int otherMapNextTile(const Sim& sim) {
    const World& w = *sim.world();
    const int start = w.startTile();
    for (int d = 0; d < 8; ++d) {
        const int tile = w.neighbour(start, d);
        if (tile >= 0 && w.canVisit(tile) && !sim.caravans().findPath(start, tile, 0).empty()) {
            return tile;
        }
    }
    return -1;
}

bool otherMapStepUntil(Sim& sim, EventType type, int limit) {
    for (int i = 0; i < limit; ++i) {
        sim.step();
        for (const Event& e : sim.events()) {
            if (e.type == type) {
                return true;
            }
        }
    }
    return false;
}

uint64_t otherMapCaravan(Sim& sim, const char* action, int caravan, int tile) {
    CaravanCommand c;
    c.action = action;
    c.caravan = caravan;
    c.tile = tile;
    return sim.submit(c);
}

bool otherMapFailed(const Sim& sim, uint64_t id, std::string* why = nullptr) {
    for (const Event& e : sim.events()) {
        if (e.type == EventType::CommandFailed && e.command == id) {
            if (why != nullptr) {
                *why = e.message;
            }
            return true;
        }
    }
    return false;
}

// Colonists of the colony on another map, by id there.
std::vector<Entity> otherMapVisitors(const Sim& map) {
    std::vector<Entity> pawns;
    for (const auto [e, c, pos] : map.things().registry().view<const Colonist, const Position>().each()) {
        (void)c;
        (void)pos;
        pawns.push_back(e);
    }
    std::sort(pawns.begin(), pawns.end());
    return pawns;
}

// The two colonists, with the meals, travel to the next tile and walk onto its map. Returns the tile.
int otherMapVisit(Sim& sim, const std::vector<Entity>& pawns, const std::vector<Entity>& items) {
    const int tile = otherMapNextTile(sim);
    if (tile < 0) {
        return -1;
    }
    CaravanCommand form;
    form.action = "Form";
    form.pawns = pawns;
    form.items = items;
    form.tile = tile;
    sim.submit(form);
    if (!otherMapStepUntil(sim, EventType::CaravanArrived, 3 * kSecondsPerDay)) {
        return -1;
    }
    const int caravan = sim.caravans().caravans().front().id;
    const uint64_t visit = otherMapCaravan(sim, "Visit", caravan, -1);
    sim.step();
    return otherMapFailed(sim, visit) ? -1 : tile;
}

}  // namespace

TEST_CASE("Other maps: a caravan visiting a tile walks onto the tile's own map, made from its site") {
    Sim sim(otherMapSettings(), otherMapDefs());
    const std::vector<Entity> pawns = otherMapPawns(sim);
    OTHERMAP_REQUIRE(pawns.size() == 2);
    std::vector<std::string> names;
    for (const Entity p : pawns) {
        names.push_back(sim.colonistOf(p)->firstName + " " + sim.colonistOf(p)->lastName);
    }
    const int tile = otherMapVisit(sim, pawns, {otherMapMeals(sim)});
    OTHERMAP_REQUIRE(tile >= 0);
    OTHERMAP_REQUIRE(sim.otherMaps().size() == 1);
    const Sim* map = sim.otherMap(tile);
    OTHERMAP_REQUIRE(map != nullptr);
    // The tile's site: its biome, climate and map seed; smaller than the colony's map, at the colony's time.
    CHECK(map->site()->tile == tile);
    CHECK(map->site()->mapSeed == sim.world()->siteFor(tile).mapSeed);
    CHECK(map->biome() == sim.world()->siteFor(tile).biome);
    const OtherMapRules rules = OtherMapRules::read(*otherMapDefs());
    CHECK(map->map().width() == (sim.world()->tile(tile).settlement != kNoSettlement ? rules.settlementMapSize : rules.mapSize));
    CHECK(map->time().seconds == sim.time().seconds);
    CHECK(map->world()->hash() == sim.world()->hash());
    // The colonists are there, on the map, with their names; here only their places are kept.
    const std::vector<Entity> visitors = otherMapVisitors(*map);
    OTHERMAP_REQUIRE(visitors.size() == 2);
    for (size_t i = 0; i < visitors.size(); ++i) {
        CHECK(std::find(names.begin(), names.end(), map->colonistOf(visitors[i])->firstName + " " + map->colonistOf(visitors[i])->lastName) != names.end());
    }
    for (const Entity p : pawns) {
        CHECK(sim.things().exists(p));
        CHECK(sim.things().registry().all_of<AwayPawn>(p));
        CHECK_FALSE(sim.things().registry().all_of<Thing>(p));
        CHECK(sim.things().registry().get<AwayPawn>(p).tile == tile);
    }
    CHECK(sim.caravans().caravans().empty());
    // The meals came along and lie on the ground there.
    bool meals = false;
    for (const auto [e, thing, pos] : map->things().registry().view<const Thing, const Position>().each()) {
        (void)e;
        (void)pos;
        meals = meals || map->defs().get(thing.def).defName() == "MealSurvivalPack";
    }
    CHECK(meals);
    Snapshot snap;
    sim.writeSnapshot(snap);
    OTHERMAP_REQUIRE(snap.otherMaps.size() == 1);
    CHECK(snap.otherMaps[0].tile == tile);
    CHECK(snap.otherMaps[0].colonists == 2);
    CHECK_FALSE(snap.otherMaps[0].name.empty());
    // Both maps run on, in step.
    sim.stepMany(600);
    CHECK(sim.otherMap(tile)->time().seconds == sim.time().seconds);
}

TEST_CASE("Other maps: colonists come home with what happened to them, and their ties at home hold") {
    Sim sim(otherMapSettings(), otherMapDefs());
    const std::vector<Entity> pawns = otherMapPawns(sim);
    OTHERMAP_REQUIRE(pawns.size() == 2);
    entt::registry& reg = sim.things().registry();
    // A colonist who stays and one who goes are lovers; the two who go are rivals of a sort (a memory).
    Entity stays = kNoEntity;
    for (const auto [e, c, pos] : reg.view<const Colonist, const Position>().each()) {
        (void)pos;
        if (std::find(pawns.begin(), pawns.end(), e) == pawns.end() && !c.protagonist) {
            stays = e;
        }
    }
    sim.step();  // every colonist gets its Social part on its first step
    OTHERMAP_REQUIRE(stays != kNoEntity && reg.all_of<Social>(stays) && reg.all_of<Social>(pawns[0]));
    reg.get<Social>(stays).bonds.push_back(Bond{BondKind::Lover, pawns[0], 0});
    reg.get<Social>(pawns[0]).bonds.push_back(Bond{BondKind::Lover, stays, 0});
    const int tile = otherMapVisit(sim, pawns, {});
    OTHERMAP_REQUIRE(tile >= 0);
    Sim* map = sim.otherMap(tile);
    OTHERMAP_REQUIRE(map != nullptr);
    // At home the lover's bond still names the one away.
    CHECK(sim.social().partner(stays) == pawns[0]);
    const std::vector<Entity> visitors = otherMapVisitors(*map);
    OTHERMAP_REQUIRE(visitors.size() == 2);
    // Over there the lover's bond to someone not there is not on the pawn.
    for (const Entity v : visitors) {
        for (const Bond& b : map->things().registry().get<Social>(v).bonds) {
            CHECK(map->things().registry().valid(b.other));
        }
    }
    // Hurt on the trip.
    DamagePawnCommand hurt;
    hurt.pawn = visitors[0];
    hurt.damageType = "Cut";
    hurt.amount = 6.0f;
    map->submit(hurt);
    sim.stepMany(60);
    const size_t woundsThere = map->things().registry().get<Health>(visitors[0]).injuries.size();
    CHECK(woundsThere > 0);
    // Home: they form a caravan on that map, walk off it and travel back.
    CaravanCommand leave;
    leave.action = "Form";
    leave.map = tile;
    leave.pawns = visitors;
    leave.tile = sim.world()->startTile();
    const uint64_t left = sim.submit(leave);
    OTHERMAP_REQUIRE(otherMapStepUntil(sim, EventType::OtherMapLeft, kSecondsPerDay));
    OTHERMAP_REQUIRE(!otherMapFailed(sim, left));
    OTHERMAP_REQUIRE(sim.caravans().caravans().size() == 1);
    CHECK(otherMapVisitors(*map).empty());
    CHECK(map->things().registry().view<const Colonist>().empty());
    OTHERMAP_REQUIRE(otherMapStepUntil(sim, EventType::CaravanEntered, 3 * kSecondsPerDay));
    // The same colonists (same ids) are home, hurt as they were, the lovers still lovers.
    for (const Entity p : pawns) {
        CHECK(sim.things().spawned(p));
        CHECK_FALSE(reg.all_of<AwayPawn>(p));
        CHECK(reg.all_of<Colonist, Health, Needs>(p));
    }
    size_t woundsHome = 0;
    for (const Entity p : pawns) {
        woundsHome += reg.get<Health>(p).injuries.size();
    }
    CHECK(woundsHome >= woundsThere);
    CHECK(sim.social().partner(stays) == pawns[0]);
    CHECK(sim.social().partner(pawns[0]) == stays);
    // The map is kept.
    CHECK(sim.otherMaps().size() == 1);
}

TEST_CASE("Other maps: a map is kept, so what was left there is there on the next visit") {
    Sim sim(otherMapSettings(), otherMapDefs());
    const std::vector<Entity> pawns = otherMapPawns(sim);
    OTHERMAP_REQUIRE(pawns.size() == 2);
    const int tile = otherMapVisit(sim, pawns, {otherMapMeals(sim)});
    OTHERMAP_REQUIRE(tile >= 0);
    const Sim* first = sim.otherMap(tile);
    const auto mealsOn = [](const Sim& map) {
        int count = 0;
        for (const auto [e, thing, pos, stack] : map.things().registry().view<const Thing, const Position, const StackCount>().each()) {
            (void)e;
            (void)pos;
            count += map.defs().get(thing.def).defName() == "MealSurvivalPack" ? stack.count : 0;
        }
        return count;
    };
    const int meals = mealsOn(*first);
    OTHERMAP_REQUIRE(meals > 0);
    // They leave without the meals and wait on the tile (no destination), then walk on again.
    CaravanCommand leave;
    leave.action = "Form";
    leave.map = tile;
    leave.pawns = otherMapVisitors(*first);
    leave.tile = -1;
    sim.submit(leave);
    OTHERMAP_REQUIRE(otherMapStepUntil(sim, EventType::OtherMapLeft, kSecondsPerDay));
    OTHERMAP_REQUIRE(sim.caravans().caravans().size() == 1);
    CHECK(sim.caravans().caravans().front().tile == tile);
    CHECK(sim.caravans().caravans().front().state == CaravanState::Waiting);
    sim.stepMany(3600);
    const int eaten = meals - mealsOn(*sim.otherMap(tile));
    CHECK(eaten >= 0);
    const uint64_t again = otherMapCaravan(sim, "Visit", sim.caravans().caravans().front().id, -1);
    sim.step();
    OTHERMAP_REQUIRE(!otherMapFailed(sim, again));
    CHECK(sim.otherMaps().size() == 1);
    CHECK(sim.otherMap(tile) == first);
    CHECK(mealsOn(*first) == meals - eaten);
    CHECK(otherMapVisitors(*first).size() == 2);
}

TEST_CASE("Other maps: Visit is refused at the colony, on the road and for a caravan that isn't there") {
    Sim sim(otherMapSettings(), otherMapDefs());
    const std::vector<Entity> pawns = otherMapPawns(sim);
    std::string why;
    const uint64_t none = otherMapCaravan(sim, "Visit", 7, -1);
    sim.step();
    CHECK(otherMapFailed(sim, none, &why));
    CHECK(why == "no such caravan");
    CaravanCommand form;
    form.action = "Form";
    form.pawns = pawns;
    form.tile = -1;
    sim.submit(form);
    OTHERMAP_REQUIRE(otherMapStepUntil(sim, EventType::CaravanDeparted, 6 * 3600));
    const uint64_t home = otherMapCaravan(sim, "Visit", 1, -1);
    sim.step();
    CHECK(otherMapFailed(sim, home, &why));
    CHECK(why == "the caravan is at the colony: use Enter");
    otherMapCaravan(sim, "Move", 1, otherMapNextTile(sim));
    sim.stepMany(10);
    if (sim.caravans().find(1)->state == CaravanState::Travelling) {
        const uint64_t road = otherMapCaravan(sim, "Visit", 1, -1);
        sim.step();
        CHECK(otherMapFailed(sim, road, &why));
        CHECK(why == "the caravan is on its way: stop it first");
    }
    CaravanCommand nowhere;
    nowhere.action = "Form";
    nowhere.map = 5;
    const uint64_t bad = sim.submit(nowhere);
    sim.step();
    CHECK(otherMapFailed(sim, bad, &why));
    CHECK(why == "no caravan has been to that tile");
    CHECK(sim.otherMaps().empty());
}

TEST_CASE("Other maps: saves during and after a visit load to the same game, and the commands replay") {
    Sim sim(otherMapSettings(), otherMapDefs());
    const std::vector<Entity> pawns = otherMapPawns(sim);
    const int tile = otherMapVisit(sim, pawns, {otherMapMeals(sim)});
    OTHERMAP_REQUIRE(tile >= 0);
    // Saved and loaded, the copy is the same game as the original, then and an hour on.
    const auto fork = [&](const char* when) {
        INFO(when);
        SaveLoadResult loaded = loadSim(saveSim(sim), otherMapDefs());
        INFO(loaded.error);
        CHECK(loaded.sim != nullptr);
        if (loaded.sim == nullptr) {
            return;
        }
        Sim& copy = *loaded.sim;
        CHECK(copy.otherMaps().size() == sim.otherMaps().size());
        CHECK(copy.stateHash() == sim.stateHash());
        CHECK(saveSim(copy) == saveSim(sim));
        sim.stepMany(3600);
        copy.stepMany(3600);
        CHECK(copy.stateHash() == sim.stateHash());
    };
    fork("on the other map");
    CaravanCommand leave;
    leave.action = "Form";
    leave.map = tile;
    leave.pawns = otherMapVisitors(*sim.otherMap(tile));
    leave.tile = sim.world()->startTile();
    sim.submit(leave);
    OTHERMAP_REQUIRE(otherMapStepUntil(sim, EventType::OtherMapLeft, kSecondsPerDay));
    fork("on the way home from it");
    OTHERMAP_REQUIRE(otherMapStepUntil(sim, EventType::CaravanEntered, 3 * kSecondsPerDay));
    fork("home again");
    // Played again from its commands, the game is the same.
    Replay read;
    std::string error;
    OTHERMAP_REQUIRE(readReplay(writeReplay(recordReplay(sim)), read, error));
    Sim again(read.settings, otherMapDefs());
    playReplay(read, again, sim.time().seconds);
    CHECK(again.stateHash() == sim.stateHash());
    CHECK(again.otherMaps().size() == 1);
}
