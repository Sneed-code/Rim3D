#include "ColonySim/Caravans.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Health.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"
#include "ColonySim/World.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// W4: caravans (docs/design/caravans.md).

// Tests build without exceptions, so a failed must-have check ends the test case by returning.
#define CARAVAN_REQUIRE(cond) \
    do {                      \
        CHECK(cond);          \
        if (!(cond)) {        \
            return;           \
        }                     \
    } while (false)

namespace {

std::shared_ptr<const DefDatabase> caravanDefs() {
    static std::shared_ptr<const DefDatabase> db;
    if (db == nullptr) {
        auto loaded = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *loaded, errors);
        db = loaded;
    }
    return db;
}

SimSettings caravanSettings() {
    SimSettings s;
    s.seed = 42;
    s.mapSize = 64;
    s.scenario = "Crashlanded";
    s.world.enabled = true;
    return s;
}

// Two colonists who aren't the protagonist, by id.
std::vector<Entity> caravanPawns(Sim& sim, size_t count = 2) {
    std::vector<Entity> pawns;
    for (const auto [e, c, pos] : sim.things().registry().view<const Colonist, const Position>().each()) {
        (void)pos;
        if (!c.protagonist) {
            pawns.push_back(e);
        }
    }
    std::sort(pawns.begin(), pawns.end());
    pawns.resize(std::min(pawns.size(), count));
    return pawns;
}

// The biggest stack of survival meals lying on the map.
Entity caravanMeals(Sim& sim) {
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

// A land tile `far` tiles or more east or west of the start tile that a caravan can reach.
int caravanFarTile(const Sim& sim, int far) {
    const World& w = *sim.world();
    const int start = w.startTile();
    for (int d = far; d < far + 20; ++d) {
        for (const int sign : {1, -1}) {
            const int x = ((w.tileX(start) + sign * d) % w.width() + w.width()) % w.width();
            const int tile = w.index(x, w.tileY(start));
            if (!sim.caravans().findPath(start, tile, 0).empty()) {
                return tile;
            }
        }
    }
    return -1;
}

uint64_t caravanForm(Sim& sim, const std::vector<Entity>& pawns, const std::vector<Entity>& items, int tile) {
    CaravanCommand c;
    c.action = "Form";
    c.pawns = pawns;
    c.items = items;
    c.tile = tile;
    return sim.submit(c);
}

bool caravanFailed(const Sim& sim, uint64_t id, std::string* why = nullptr) {
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

// Steps until the caravan has left the map (or `limit` steps).
bool caravanStepUntilDeparted(Sim& sim, int limit = 6 * 3600) {
    for (int i = 0; i < limit; ++i) {
        sim.step();
        for (const Event& e : sim.events()) {
            if (e.type == EventType::CaravanDeparted) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

TEST_CASE("Caravans: a tile's cost is its biome, hills, cold and road, by the wiki's numbers") {
    Sim sim(caravanSettings(), caravanDefs());
    CARAVAN_REQUIRE(sim.world() != nullptr);
    const World& w = *sim.world();
    const CaravanSystem& caravans = sim.caravans();
    const double perTile = rimWorldTicksToSeconds(4412.0);  // 13.6 tiles a day of travel at difficulty 1
    bool flatForest = false;
    bool hills = false;
    bool road = false;
    bool blocked = false;
    for (int i = 0; i < w.tileCount(); ++i) {
        const WorldTile& t = w.tile(i);
        if (t.water || t.hilliness == Hilliness::Impassable) {
            if (!blocked) {
                CHECK(caravans.difficulty(i, 1) == 0.0);
                blocked = true;
            }
            continue;
        }
        const double summer = caravans.difficulty(i, 1);  // nobody freezes in a temperate summer
        if (!flatForest && w.biomeName(t) == "TemperateForest" && t.hilliness == Hilliness::Flat && t.temperature > 5.0f) {
            CHECK(summer == doctest::Approx(1.0));
            flatForest = true;
        }
        if (!hills && w.biomeName(t) == "TemperateForest" && t.hilliness == Hilliness::LargeHills && t.temperature > 5.0f) {
            CHECK(summer == doctest::Approx(2.5));
            hills = true;
        }
    }
    CHECK(flatForest);
    CHECK(hills);
    CHECK(blocked);
    // A step along a road costs half of one beside it; a diagonal step the square root of 2 more.
    for (const WorldRoad& r : w.roads()) {
        if (caravans.difficulty(r.tileB, 1) <= 0.0) {
            continue;
        }
        bool diagonal = false;
        for (int d = 0; d < 8; ++d) {
            if (w.neighbour(r.tileA, d) == r.tileB) {
                diagonal = d % 2 == 1;
            }
        }
        const double expected = perTile * caravans.difficulty(r.tileB, 1) * 0.5 * (diagonal ? 1.41421356 : 1.0);
        CHECK(caravans.stepSeconds(r.tileA, r.tileB, 1) == doctest::Approx(expected));
        road = true;
        break;
    }
    CHECK(road);
    // An ice sheet in winter: 1.5 for the ice, up to 2 more for the cold.
    for (int i = 0; i < w.tileCount(); ++i) {
        const WorldTile& t = w.tile(i);
        if (w.biomeName(t) == "IceSheet" && t.hilliness == Hilliness::Flat && !t.water && t.temperature < -30.0f) {
            CHECK(caravans.difficulty(i, 3) == doctest::Approx(3.5));
            break;
        }
    }
}

TEST_CASE("Caravans: forming is refused without a world, for non-colonists and for too much to carry") {
    SimSettings noWorld = caravanSettings();
    noWorld.world.enabled = false;
    Sim plain(noWorld, caravanDefs());
    const uint64_t a = caravanForm(plain, caravanPawns(plain), {}, -1);
    plain.step();
    std::string why;
    CHECK(caravanFailed(plain, a, &why));
    CHECK(why.find("world") != std::string::npos);

    Sim sim(caravanSettings(), caravanDefs());
    const Entity meals = caravanMeals(sim);
    CARAVAN_REQUIRE(meals != kNoEntity);
    const uint64_t b = caravanForm(sim, {meals}, {}, -1);
    sim.step();
    CHECK(caravanFailed(sim, b, &why));
    CHECK(why.find("colonists") != std::string::npos);

    // Steel weighs 0.5 kg; the 450 of it lying near the colonists is more than one colonist's 35 kg.
    std::vector<Entity> steel;
    for (const auto [e, thing, pos] : sim.things().registry().view<const Thing, const Position>().each()) {
        (void)pos;
        if (sim.defs().get(thing.def).defName() == "Steel") {
            steel.push_back(e);
        }
    }
    std::sort(steel.begin(), steel.end());
    CARAVAN_REQUIRE(!steel.empty());
    const uint64_t c = caravanForm(sim, caravanPawns(sim, 1), steel, -1);
    sim.step();
    CHECK(caravanFailed(sim, c, &why));
    CHECK(why.find("too heavy") != std::string::npos);
    CHECK(sim.caravans().caravans().empty());
}

TEST_CASE("Caravans: members walk off the map, travel the world, eat, sleep at night and arrive") {
    Sim sim(caravanSettings(), caravanDefs());
    CARAVAN_REQUIRE(sim.setupErrors().empty());
    const std::vector<Entity> pawns = caravanPawns(sim);
    const Entity meals = caravanMeals(sim);
    CARAVAN_REQUIRE(pawns.size() == 2);
    CARAVAN_REQUIRE(meals != kNoEntity);
    const int mealsBefore = sim.things().registry().get<StackCount>(meals).count;
    const int far = caravanFarTile(sim, 3);
    CARAVAN_REQUIRE(far >= 0);
    caravanForm(sim, pawns, {meals}, far);
    sim.step();
    CARAVAN_REQUIRE(sim.caravans().caravans().size() == 1);
    CHECK(sim.caravans().caravans()[0].state == CaravanState::Forming);
    CHECK(sim.things().registry().all_of<CaravanMember>(pawns[0]));

    CARAVAN_REQUIRE(caravanStepUntilDeparted(sim));
    const Caravan* c = &sim.caravans().caravans()[0];
    CHECK(c->state != CaravanState::Forming);
    CHECK(c->tile == sim.world()->startTile());
    for (const Entity pawn : pawns) {
        CHECK_FALSE(sim.things().spawned(pawn));
        CHECK(sim.things().registry().all_of<CaravanMember>(pawn));
    }
    CHECK_FALSE(sim.things().spawned(meals));
    CHECK(c->items.size() == 1);
    CHECK(sim.caravans().massOf(*c) > 0.0);
    CHECK(sim.caravans().capacityOf(*c) == doctest::Approx(70.0));

    Snapshot snap;
    sim.writeSnapshot(snap);
    CARAVAN_REQUIRE(snap.caravans.size() == 1);
    CHECK(snap.caravans[0].pawns == 2);
    CHECK(snap.find(pawns[0]) == nullptr);  // off the map, out of the snapshot's things

    // A hungry member eats from what the caravan carries.
    sim.things().registry().get<Needs>(pawns[0]).food = 0.1f;
    sim.step();
    CHECK(sim.things().registry().get<Needs>(pawns[0]).food > 0.5f);
    CHECK(sim.things().registry().get<StackCount>(meals).count < mealsBefore);

    // Over the next night it rests (and its members' rest rises); by day it moves on, and gets there.
    bool rested = false;
    bool restRose = false;
    bool arrived = false;
    float lastRest = sim.things().registry().get<Needs>(pawns[1]).rest;
    for (int i = 0; i < 3 * kSecondsPerDay && !arrived; ++i) {
        sim.step();
        c = sim.caravans().find(1);
        CARAVAN_REQUIRE(c != nullptr);
        const float rest = sim.things().registry().get<Needs>(pawns[1]).rest;
        if (c->state == CaravanState::Resting) {
            rested = true;
            CHECK((sim.time().hourOfDay() >= 22 || sim.time().hourOfDay() < 6));
            restRose = restRose || rest > lastRest;
        }
        lastRest = rest;
        for (const Event& e : sim.events()) {
            arrived = arrived || (e.type == EventType::CaravanArrived && static_cast<int>(e.amount) == far);
        }
    }
    CHECK(rested);
    CHECK(restRose);
    CHECK(arrived);
    c = sim.caravans().find(1);
    CARAVAN_REQUIRE(c != nullptr);
    CHECK(c->tile == far);
    CHECK(c->state == CaravanState::Waiting);
}

TEST_CASE("Caravans: a caravan sent home walks back onto the map with what it carries") {
    Sim sim(caravanSettings(), caravanDefs());
    const std::vector<Entity> pawns = caravanPawns(sim);
    const Entity meals = caravanMeals(sim);
    const int start = sim.world()->startTile();
    const int next = caravanFarTile(sim, 1);
    CARAVAN_REQUIRE(next >= 0);
    caravanForm(sim, pawns, {meals}, next);
    CARAVAN_REQUIRE(caravanStepUntilDeparted(sim));
    bool arrived = false;
    for (int i = 0; i < 2 * kSecondsPerDay && !arrived; ++i) {
        sim.step();
        for (const Event& e : sim.events()) {
            arrived = arrived || e.type == EventType::CaravanArrived;
        }
    }
    CARAVAN_REQUIRE(arrived);
    CaravanCommand back;
    back.action = "Move";
    back.caravan = 1;
    back.tile = start;
    sim.submit(back);
    bool entered = false;
    for (int i = 0; i < 2 * kSecondsPerDay && !entered; ++i) {
        sim.step();
        for (const Event& e : sim.events()) {
            entered = entered || e.type == EventType::CaravanEntered;
        }
    }
    CHECK(entered);
    CHECK(sim.caravans().caravans().empty());
    for (const Entity pawn : pawns) {
        CHECK(sim.things().spawned(pawn));
        CHECK_FALSE(sim.things().registry().all_of<CaravanMember>(pawn));
    }
    CHECK(sim.things().spawned(meals));
    // Back on the map they work again.
    sim.stepMany(600);
    bool working = false;
    for (const Entity pawn : pawns) {
        const Position& pos = sim.things().registry().get<Position>(pawn);
        working = working || pos.x > 3.0f;
    }
    CHECK(working);
}

TEST_CASE("Caravans: a member who dies on the road is carried as a corpse; with nobody left the caravan is lost") {
    Sim sim(caravanSettings(), caravanDefs());
    const std::vector<Entity> pawns = caravanPawns(sim);
    caravanForm(sim, pawns, {}, -1);
    CARAVAN_REQUIRE(caravanStepUntilDeparted(sim));
    sim.health().kill(pawns[0], DeathCause::Starvation);
    sim.step();
    const Caravan* c = sim.caravans().find(1);
    CARAVAN_REQUIRE(c != nullptr);
    CHECK(c->members.size() == 1);
    CHECK(std::find(c->items.begin(), c->items.end(), pawns[0]) != c->items.end());
    sim.health().kill(pawns[1], DeathCause::Starvation);
    sim.step();
    CHECK(sim.caravans().caravans().empty());
    bool lost = false;
    for (const Event& e : sim.events()) {
        lost = lost || e.type == EventType::CaravanLost;
    }
    CHECK(lost);
    CHECK_FALSE(sim.things().exists(pawns[1]));
}

TEST_CASE("Caravans: a save on the road and a save while forming load to the same game") {
    for (const bool onRoad : {false, true}) {
        Sim original(caravanSettings(), caravanDefs());
        caravanForm(original, caravanPawns(original), {caravanMeals(original)}, caravanFarTile(original, 3));
        if (onRoad) {
            CARAVAN_REQUIRE(caravanStepUntilDeparted(original));
            original.stepMany(3600);
        } else {
            original.stepMany(30);
            CARAVAN_REQUIRE(!original.caravans().caravans().empty());
            CARAVAN_REQUIRE(original.caravans().caravans()[0].state == CaravanState::Forming);
        }
        SaveLoadResult loaded = loadSim(saveSim(original), caravanDefs());
        CARAVAN_REQUIRE(loaded.sim != nullptr);
        Sim& copy = *loaded.sim;
        CHECK(copy.caravans().caravans().size() == 1);
        CHECK(copy.stateHash() == original.stateHash());
        original.stepMany(4 * 3600);
        copy.stepMany(4 * 3600);
        CHECK(copy.stateHash() == original.stateHash());
    }
}

TEST_CASE("Caravans: the Caravan command goes through a replay") {
    Sim sim(caravanSettings(), caravanDefs());
    const std::vector<Entity> pawns = caravanPawns(sim);
    const Entity meals = caravanMeals(sim);
    const int far = caravanFarTile(sim, 3);
    caravanForm(sim, pawns, {meals}, far);
    sim.stepMany(100);
    Replay read;
    std::string error;
    CARAVAN_REQUIRE(readReplay(writeReplay(recordReplay(sim)), read, error));
    CARAVAN_REQUIRE(read.commands.size() == 1);
    const auto* c = std::get_if<CaravanCommand>(&read.commands[0].command);
    CARAVAN_REQUIRE(c != nullptr);
    CHECK(c->action == "Form");
    CHECK(c->tile == far);
    CHECK(c->pawns == pawns);
    CHECK(c->items == std::vector<Entity>{meals});
    // Played again, it gives the same game.
    Sim again(read.settings, caravanDefs());
    playReplay(read, again, sim.time().seconds);
    CHECK(again.stateHash() == sim.stateHash());
}
