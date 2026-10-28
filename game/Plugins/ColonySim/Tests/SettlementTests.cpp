#include "ColonySim/Commands.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"
#include "ColonySim/Trade.h"
#include "ColonySim/World.h"

#include <doctest.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// W6: a settlement on its map (docs/design/settlements.md).

#define SETTLEMENT_REQUIRE(cond) \
    do {                         \
        CHECK((cond));           \
        if (!(cond)) {           \
            return;              \
        }                        \
    } while (false)

namespace {

std::shared_ptr<const DefDatabase> settlementDefs() {
    static std::shared_ptr<const DefDatabase> db;
    if (db == nullptr) {
        auto loaded = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *loaded, errors);
        db = loaded;
    }
    return db;
}

SimSettings settlementColonySettings() {
    SimSettings s;
    s.seed = 42;
    s.mapSize = 64;
    s.scenario = "Crashlanded";
    s.world.enabled = true;
    return s;
}

// The tile of the first settlement of `faction` on the colony's world, or -1.
int settlementTileOf(const Sim& colony, const std::string& faction) {
    for (const WorldSettlement& s : colony.world()->settlements()) {
        if (s.faction == faction) {
            return s.tile;
        }
    }
    return -1;
}

// That tile's map as the colony makes it (OtherMaps.cpp): a sim of its own, its goodwill the colony's, then the base.
std::unique_ptr<Sim> settlementMap(Sim& colony, int tile) {
    SimSettings s;
    s.seed = 7;
    s.mapSize = 384;
    s.world = colony.settings().world;
    s.world.startTile = tile;
    s.world.visit = true;
    s.startSecond = colony.time().seconds;
    auto map = std::make_unique<Sim>(s, settlementDefs());
    map->trade().setGoodwillOwner(&colony.trade());
    std::string error;
    CHECK(map->buildSettlement(error));
    return map;
}

const Visit* settlementVisit(const Sim& map) {
    for (const Visit& v : map.trade().state().visits) {
        if (v.kind == VisitKind::Settlement) {
            return &v;
        }
    }
    return nullptr;
}

int settlementCount(const Sim& map, const char* defName) {
    int n = 0;
    const entt::registry& reg = map.things().registry();
    for (const auto [e, thing, pos] : reg.view<const Thing, const Position>().each()) {
        (void)pos;
        if (thing.category == ThingCategory::Building && !reg.all_of<ConstructionSite>(e) && map.defs().get(thing.def).defName() == defName) {
            ++n;
        }
    }
    return n;
}

}  // namespace

TEST_CASE("a friendly settlement's map has its base, its people and a trader") {
    Sim colony(settlementColonySettings(), settlementDefs());
    SETTLEMENT_REQUIRE(colony.world() != nullptr);
    const int tile = settlementTileOf(colony, "FrayCompact");
    SETTLEMENT_REQUIRE(tile >= 0);
    auto map = settlementMap(colony, tile);

    // Walls round a perimeter and rooms (an industrial faction), doors, beds, furniture, a campfire in the yard.
    CHECK(settlementCount(*map, "Wall") >= 100);
    CHECK(settlementCount(*map, "Door") >= 4);
    CHECK(settlementCount(*map, "Bed") >= 2);
    CHECK(settlementCount(*map, "Campfire") == 1);
    const Visit* visit = settlementVisit(*map);
    SETTLEMENT_REQUIRE(visit != nullptr);
    CHECK(visit->phase == VisitPhase::Staying);
    CHECK(visit->faction == "FrayCompact");
    CHECK(visit->members.size() >= 8);
    CHECK(visit->members.size() <= 12);
    SETTLEMENT_REQUIRE(visit->trader != kNoEntity);
    CHECK(!visit->traderKind.empty());
    CHECK(!visit->stock.empty());
    CHECK(visit->silver > 0);
    const entt::registry& reg = map->things().registry();
    for (const Entity m : visit->members) {
        CHECK(reg.all_of<Visitor>(m));
        CHECK(map->fighters().factionOf(m) != kPlayerFaction);
    }

    // They live there: six hours on they are all still on the map, within the base.
    const uint32_t id = visit->id;
    const size_t residents = visit->members.size();
    const CellPos spot = visit->spot;
    const int range = visit->range;
    for (int i = 0; i < 6 * kSecondsPerHour; ++i) {
        map->step();
    }
    visit = settlementVisit(*map);
    SETTLEMENT_REQUIRE(visit != nullptr);
    CHECK(visit->id == id);
    CHECK(visit->members.size() == residents);
    for (const Entity m : visit->members) {
        const CellPos c = cellOf(reg.get<Position>(m));
        CHECK(std::abs(c.x - spot.x) <= range + 4);
        CHECK(std::abs(c.y - spot.y) <= range + 4);
    }
}

TEST_CASE("the same settlement is always laid out the same way") {
    Sim colony(settlementColonySettings(), settlementDefs());
    const int tile = settlementTileOf(colony, "FrayCompact");
    SETTLEMENT_REQUIRE(tile >= 0);
    auto a = settlementMap(colony, tile);
    auto b = settlementMap(colony, tile);
    CHECK(a->stateHash() == b->stateHash());
    CHECK(settlementCount(*a, "Wall") == settlementCount(*b, "Wall"));
}

TEST_CASE("a tribe's settlement has no wall round it") {
    Sim colony(settlementColonySettings(), settlementDefs());
    const int fray = settlementTileOf(colony, "FrayCompact");
    const int clan = settlementTileOf(colony, "ClanGentle");
    SETTLEMENT_REQUIRE(fray >= 0);
    SETTLEMENT_REQUIRE(clan >= 0);
    auto walled = settlementMap(colony, fray);
    auto tribe = settlementMap(colony, clan);
    CHECK(settlementCount(*tribe, "Wall") > 0);
    CHECK(settlementCount(*tribe, "Wall") < settlementCount(*walled, "Wall"));
    const Visit* visit = settlementVisit(*tribe);
    SETTLEMENT_REQUIRE(visit != nullptr);
    CHECK(visit->trader != kNoEntity);
}

TEST_CASE("a hostile faction's settlement defends itself and won't trade") {
    Sim colony(settlementColonySettings(), settlementDefs());
    // The first settlement of a faction that is the colony's enemy (pirates or a savage clan).
    int tile = -1;
    for (const WorldSettlement& w : colony.world()->settlements()) {
        if (tile < 0 && colony.trade().hostile(w.faction) && colony.defs().find("Factions", w.faction)->count("pawnKinds") > 0) {
            tile = w.tile;
        }
    }
    SETTLEMENT_REQUIRE(tile >= 0);
    auto map = settlementMap(colony, tile);
    const Visit* visit = settlementVisit(*map);
    SETTLEMENT_REQUIRE(visit != nullptr);
    CHECK(visit->phase == VisitPhase::Defending);
    CHECK(visit->trader == kNoEntity);
    CHECK(visit->members.size() >= 8);
    const entt::registry& reg = map->things().registry();
    for (const Entity m : visit->members) {
        CHECK(!reg.all_of<Visitor>(m));
        SETTLEMENT_REQUIRE(reg.all_of<Fighter>(m));
        CHECK(reg.get<Fighter>(m).mode == FightMode::Fight);
    }
    std::string why;
    CHECK(!map->trade().gift(visit->id, {TradeLine{"Silver", 1}}, why));
    CHECK(why == "they are fighting the colony");
}

TEST_CASE("a settlement shares the colony's goodwill and turns on it when it is hostile") {
    Sim colony(settlementColonySettings(), settlementDefs());
    const int tile = settlementTileOf(colony, "FrayCompact");
    SETTLEMENT_REQUIRE(tile >= 0);
    auto map = settlementMap(colony, tile);
    CHECK(map->trade().goodwill("FrayCompact") == colony.trade().goodwill("FrayCompact"));

    // A gift on that map is the colony's goodwill.
    const float before = colony.trade().goodwill("FrayCompact");
    const Visit* visit = settlementVisit(*map);
    SETTLEMENT_REQUIRE(visit != nullptr);
    map->things().spawn("Silver", CellPos{visit->spot.x + 1, visit->spot.y + 1, 0}, 400);
    std::string why;
    CHECK(map->trade().gift(visit->id, {TradeLine{"Silver", 400}}, why));
    CHECK(colony.trade().goodwill("FrayCompact") == doctest::Approx(before + 10.0f));

    // The colony falls out with them: the settlement's people fight from then on.
    CHECK(colony.trade().adjustGoodwill("FrayCompact", -200.0f, why));
    for (int i = 0; i < 30; ++i) {
        map->step();
    }
    visit = settlementVisit(*map);
    SETTLEMENT_REQUIRE(visit != nullptr);
    CHECK(visit->phase == VisitPhase::Defending);
    for (const Entity m : visit->members) {
        CHECK(!map->things().registry().all_of<Visitor>(m));
    }
}

TEST_CASE("the colony trades with a settlement's trader at a little better prices") {
    Sim colony(settlementColonySettings(), settlementDefs());
    const int tile = settlementTileOf(colony, "FrayCompact");
    SETTLEMENT_REQUIRE(tile >= 0);
    auto map = settlementMap(colony, tile);
    const Visit* visit = settlementVisit(*map);
    SETTLEMENT_REQUIRE(visit != nullptr);
    const CellPos near{visit->spot.x + 2, visit->spot.y + 2, 0};
    const Entity negotiator = map->things().spawn("Human", near);
    SETTLEMENT_REQUIRE(negotiator != kNoEntity);
    map->things().spawn("MealSurvivalPack", CellPos{near.x + 1, near.y, 0}, 10);
    const int silverBefore = map->trade().silverOnMap();
    TradeCommand deal;
    deal.visit = visit->id;
    deal.negotiator = negotiator;
    deal.sell.push_back(TradeLine{"MealSurvivalPack", 10});
    map->submit(deal);
    map->step();
    const Def& meal = *map->defs().find("Things", "MealSurvivalPack");
    const float improvement = map->trade().priceImprovement(negotiator);
    const int expected = static_cast<int>(10.0f * map->trade().unitPrice(meal, false, improvement + map->trade().rules().settlementDiscount) + 1e-4f);
    CHECK(map->trade().silverOnMap() - silverBefore == expected);
    CHECK(expected > static_cast<int>(10.0f * map->trade().unitPrice(meal, false, improvement) + 1e-4f));
}

TEST_CASE("a settlement's people and base come back from a save") {
    Sim colony(settlementColonySettings(), settlementDefs());
    const int tile = settlementTileOf(colony, "FrayCompact");
    SETTLEMENT_REQUIRE(tile >= 0);
    auto map = settlementMap(colony, tile);
    for (int i = 0; i < 120; ++i) {
        map->step();
    }
    SaveLoadResult loaded = loadSim(saveSim(*map), settlementDefs());
    SETTLEMENT_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == map->stateHash());
    const Visit* before = settlementVisit(*map);
    const Visit* after = settlementVisit(*loaded.sim);
    SETTLEMENT_REQUIRE(before != nullptr);
    SETTLEMENT_REQUIRE(after != nullptr);
    CHECK(after->restockAt == before->restockAt);
    CHECK(after->range == before->range);
    CHECK(after->members.size() == before->members.size());
    CHECK(settlementCount(*loaded.sim, "Bed") == settlementCount(*map, "Bed"));
}

TEST_CASE("a trade or gift on another map names its tile, and replays keep it") {
    TradeCommand deal;
    deal.visit = 3;
    deal.map = 1234;
    deal.sell.push_back(TradeLine{"Silver", 1});
    GiftCommand gift;
    gift.visit = 3;
    gift.map = 1234;
    gift.items.push_back(TradeLine{"Silver", 1});
    Replay replay;
    replay.settings = settlementColonySettings();
    replay.commands.push_back(LoggedCommand{10, deal});
    replay.commands.push_back(LoggedCommand{11, gift});
    Replay back;
    std::string error;
    SETTLEMENT_REQUIRE(readReplay(writeReplay(replay), back, error));
    SETTLEMENT_REQUIRE(back.commands.size() == 2);
    const auto* d = std::get_if<TradeCommand>(&back.commands[0].command);
    const auto* g = std::get_if<GiftCommand>(&back.commands[1].command);
    SETTLEMENT_REQUIRE(d != nullptr);
    SETTLEMENT_REQUIRE(g != nullptr);
    CHECK(d->map == 1234);
    CHECK(g->map == 1234);

    // On the colony with no map of that tile, it is refused.
    Sim colony(settlementColonySettings(), settlementDefs());
    const uint64_t id = colony.submit(deal);
    colony.step();
    bool refused = false;
    for (const Event& e : colony.events()) {
        refused = refused || (e.type == EventType::CommandFailed && e.command == id && e.message == "no caravan has been to that tile");
    }
    CHECK(refused);
}
