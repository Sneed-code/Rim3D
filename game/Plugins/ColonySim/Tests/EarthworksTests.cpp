#include "ColonySim/Commands.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Designations.h"
#include "ColonySim/Events.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Earthworks, ground fit and the stuck watchdog (docs/design/earthworks.md).

#define EARTH_REQUIRE(...)     \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> earthDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

// A hillside: the ground rises `rise` metres a metre along x, plus a little along y. No plants.
std::unique_ptr<Sim> earthSim(float rise = 0.13f) {
    auto sim = std::make_unique<Sim>(SimSettings{5, 48, 0, 0, true}, earthDefs());
    for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < 48; ++x) {
            sim->editMap().setGroundHeight(CellPos{x, y, 0}, 2.0f + rise * static_cast<float>(x) + 0.04f * static_cast<float>(y));
            sim->plants().remove(CellPos{x, y, 0});
        }
    }
    return sim;
}

Entity earthSpawn(Sim& sim, const std::string& def, CellPos cell, int count = 1) {
    sim.submit(SpawnThingCommand{def, cell, count});
    sim.step();
    for (const Event& e : sim.events()) {
        if (e.type == EventType::ThingSpawned && e.cell == cell && sim.defs().get(e.def).defName() == def) {
            return e.thing;
        }
    }
    return kNoEntity;
}

std::string earthLastError(const Sim& sim) {
    for (const Event& e : sim.events()) {
        if (e.type == EventType::CommandFailed) {
            return e.message;
        }
    }
    return {};
}

bool earthFlat(const Sim& sim, int x, int y, float base) {
    const Map& m = sim.map();
    return m.groundHeight(x, y) == base && m.groundHeight(x + 1, y) == base && m.groundHeight(x, y + 1) == base &&
           m.groundHeight(x + 1, y + 1) == base;
}

template <typename F>
int earthRunUntil(Sim& sim, int limit, F done) {
    for (int i = 0; i < limit; ++i) {
        if (done()) {
            return i;
        }
        sim.step();
    }
    return done() ? limit : -1;
}

}  // namespace

TEST_CASE("Earthworks: a plan marks its ground but moves none of it; each unit moves one corner a quarter metre") {
    std::unique_ptr<Sim> owned = earthSim(0.5f);
    Sim& sim = *owned;
    const float before = sim.map().groundHeight(21, 21);
    sim.submit(PlaceBlueprintCommand{"Wall", "WoodLog", CellPos{20, 20, 0}, Rot4::North});
    sim.step();
    EARTH_REQUIRE(sim.construction().siteAt(CellPos{20, 20, 0}) != kNoEntity);
    CHECK(sim.map().levelled(CellPos{20, 20, 0}));  // the base is planned, so later plans join it
    CHECK(sim.map().groundHeight(21, 21) == before);
    CHECK(sim.map().earthPending(CellPos{20, 20, 0}));
    const CellRect rect{20, 20, 1, 1, 0};
    const int units = sim.map().earthUnits(rect);
    // Corners 0.5 m apart along x: the high ones are two units above the base.
    CHECK(units >= 2);
    const float base = sim.map().levelBase(CellPos{20, 20, 0});
    Map& map = sim.editMap();
    int done = 0;
    while (map.digEarth(CellPos{20, 20, 0}) >= 0) {
        ++done;
        CHECK(map.earthUnits(rect) == units - done);
    }
    CHECK(done == units);
    CHECK(earthFlat(sim, 20, 20, base));
    CHECK_FALSE(sim.map().earthPending(CellPos{20, 20, 0}));
}

TEST_CASE("Earthworks: the builder digs the footprint first, then builds; the ground only moves once work starts") {
    std::unique_ptr<Sim> owned = earthSim(0.5f);
    Sim& sim = *owned;
    earthSpawn(sim, "Human", CellPos{10, 10, 0});
    sim.submit(PlaceBlueprintCommand{"Wall", "WoodLog", CellPos{14, 10, 0}, Rot4::North});
    sim.step();
    const Entity site = sim.construction().siteAt(CellPos{14, 10, 0});
    EARTH_REQUIRE(site != kNoEntity);
    const float corner = sim.map().groundHeight(15, 11);
    sim.stepMany(200);  // no wood yet: nothing is dug
    CHECK(sim.map().groundHeight(15, 11) == corner);
    earthSpawn(sim, "WoodLog", CellPos{8, 8, 0}, 5);
    EARTH_REQUIRE(earthRunUntil(sim, 4000, [&] { return sim.map().passage(CellPos{14, 10, 0}) == Passage::Blocked; }) >= 0);
    const float base = sim.map().levelBase(CellPos{14, 10, 0});
    CHECK(earthFlat(sim, 14, 10, base));
    CHECK(sim.map().earthCorners().empty());
}

TEST_CASE("Earthworks: sandbags lean with the slope; furniture won't go on a slope") {
    std::unique_ptr<Sim> owned = earthSim(0.5f);
    Sim& sim = *owned;
    const DefDatabase& defs = sim.defs();
    CHECK(groundFitOf(defs.get(defs.idOf("Things", "Sandbags"))) == GroundFit::Conform);
    CHECK(groundFitOf(defs.get(defs.idOf("Things", "Barricade"))) == GroundFit::Conform);
    CHECK(groundFitOf(defs.get(defs.idOf("Things", "Wall"))) == GroundFit::Level);
    CHECK(groundFitOf(defs.get(defs.idOf("Things", "Bed"))) == GroundFit::Flat);

    sim.submit(PlaceBlueprintCommand{"Sandbags", "Cloth", CellPos{20, 20, 0}, Rot4::North});
    sim.step();
    CHECK(sim.construction().siteAt(CellPos{20, 20, 0}) != kNoEntity);
    CHECK_FALSE(sim.map().levelled(CellPos{20, 20, 0}));
    CHECK_FALSE(sim.map().earthPending(CellPos{20, 20, 0}));
    // Built straight away (a spawn), they don't dig either.
    const float h = sim.map().groundHeight(31, 31);
    EARTH_REQUIRE(earthSpawn(sim, "Sandbags", CellPos{30, 30, 0}) != kNoEntity);
    CHECK(sim.map().groundHeight(31, 31) == h);

    sim.submit(PlaceBlueprintCommand{"Bed", "WoodLog", CellPos{25, 25, 0}, Rot4::North});
    sim.step();
    CHECK(sim.construction().siteAt(CellPos{25, 25, 0}) == kNoEntity);
    CHECK(earthLastError(sim) == "too steep: level the earth first");

    // Levelled first (even only planned), it fits.
    sim.submit(LevelEarthCommand{CellRect{24, 24, 4, 4, 0}, static_cast<int>(std::lround(sim.map().groundHeight(25, 25) * 100.0f))});
    sim.step();
    CHECK(earthLastError(sim).empty());
    sim.submit(PlaceBlueprintCommand{"Bed", "WoodLog", CellPos{25, 25, 0}, Rot4::North});
    sim.step();
    CHECK(sim.construction().siteAt(CellPos{25, 25, 0}) != kNoEntity);
}

TEST_CASE("Earthworks: ground between walls (inside an L) is planned flat with them, outside is not") {
    std::unique_ptr<Sim> owned = earthSim(0.1f);
    Sim& sim = *owned;
    for (int x = 10; x <= 15; ++x) {
        sim.submit(PlaceBlueprintCommand{"Wall", "WoodLog", CellPos{x, 10, 0}, Rot4::North});
    }
    for (int y = 11; y <= 15; ++y) {
        sim.submit(PlaceBlueprintCommand{"Wall", "WoodLog", CellPos{10, y, 0}, Rot4::North});
    }
    sim.step();
    CHECK(sim.map().levelled(CellPos{12, 12, 0}));
    CHECK(sim.map().levelled(CellPos{14, 14, 0}));
    CHECK(sim.map().earthPending(CellPos{12, 12, 0}));
    CHECK_FALSE(sim.map().levelled(CellPos{12, 8, 0}));  // the far side of the long arm
    CHECK_FALSE(sim.map().levelled(CellPos{8, 12, 0}));  // the far side of the short arm
    CHECK_FALSE(sim.map().levelled(CellPos{20, 20, 0}));
}

TEST_CASE("Earthworks: Level earth is dug by a colonist, outside in; plants on moved ground go, trees get marked") {
    std::unique_ptr<Sim> owned = earthSim(0.2f);
    Sim& sim = *owned;
    const DefDatabase& defs = sim.defs();
    const DefId grass = defs.idOf("Things", "Plant_Grass");
    const DefId tree = defs.idOf("Things", "Plant_TreeOak");
    EARTH_REQUIRE(grass.valid());
    sim.plants().place(CellPos{21, 21, 0}, grass, 1.0f);
    if (tree.valid()) {
        sim.plants().place(CellPos{23, 23, 0}, tree, 1.0f);
    }
    earthSpawn(sim, "Human", CellPos{10, 10, 0});
    const int baseCm = static_cast<int>(std::lround(sim.map().groundHeight(20, 20) * 100.0f));
    sim.submit(LevelEarthCommand{CellRect{20, 20, 4, 4, 0}, baseCm});
    sim.step();
    EARTH_REQUIRE(earthLastError(sim).empty());
    CHECK(sim.map().earthPending(CellPos{21, 21, 0}));
    CHECK(sim.map().groundHeight(22, 22) != doctest::Approx(static_cast<float>(baseCm) / 100.0f));
    sim.step();
    if (tree.valid()) {
        CHECK(sim.designations().at(CellPos{23, 23, 0}) == DesignationKind::CutPlant);
    }
    EARTH_REQUIRE(earthRunUntil(sim, 20000, [&] { return sim.map().earthCorners().empty(); }) >= 0);
    const float base = std::round(static_cast<float>(baseCm) / 100.0f / Map::kLevelStepM) * Map::kLevelStepM;
    CHECK(earthFlat(sim, 21, 21, base));
    CHECK(earthFlat(sim, 22, 22, base));
    CHECK_FALSE(sim.plants().has(CellPos{21, 21, 0}));
}

TEST_CASE("Earthworks: Level earth refuses more than twice a wall's height; cancelling drops the plan") {
    std::unique_ptr<Sim> owned = earthSim(0.1f);
    Sim& sim = *owned;
    sim.submit(LevelEarthCommand{CellRect{20, 20, 2, 2, 0}, static_cast<int>(std::lround((sim.map().groundHeight(20, 20) + 7.0f) * 100.0f))});
    sim.step();
    CHECK_FALSE(earthLastError(sim).empty());
    CHECK(sim.map().earthCorners().empty());

    sim.submit(LevelEarthCommand{CellRect{20, 20, 3, 3, 0}, static_cast<int>(std::lround((sim.map().groundHeight(20, 20) + 1.0f) * 100.0f))});
    sim.step();
    CHECK_FALSE(sim.map().earthCorners().empty());
    sim.submit(CancelEarthworksCommand{CellRect{20, 20, 3, 3, 0}});
    sim.step();
    CHECK(sim.map().earthCorners().empty());
    CHECK_FALSE(sim.map().levelled(CellPos{21, 21, 0}));

    // A cancelled blueprint drops its earthworks too.
    sim.submit(PlaceBlueprintCommand{"Wall", "WoodLog", CellPos{30, 30, 0}, Rot4::North});
    sim.step();
    const Entity site = sim.construction().siteAt(CellPos{30, 30, 0});
    EARTH_REQUIRE(site != kNoEntity);
    CHECK_FALSE(sim.map().earthCorners().empty());
    sim.submit(CancelBuildCommand{site});
    sim.step();
    CHECK(sim.map().earthCorners().empty());
}

TEST_CASE("Earthworks: saved and loaded, the planned corners come back") {
    std::unique_ptr<Sim> owned = earthSim(0.3f);
    Sim& sim = *owned;
    sim.submit(LevelEarthCommand{CellRect{20, 20, 3, 3, 0}, static_cast<int>(std::lround(sim.map().groundHeight(20, 20) * 100.0f))});
    sim.step();
    sim.editMap().digEarth(CellPos{21, 21, 0});
    const Map::EarthState before = sim.map().earthState();
    EARTH_REQUIRE(!before.corners.empty());
    SaveLoadResult loaded = loadSim(saveSim(sim), earthDefs());
    EARTH_REQUIRE(loaded.sim != nullptr);
    const Map::EarthState after = loaded.sim->map().earthState();
    CHECK(after.corners == before.corners);
    CHECK(after.targetCm == before.targetCm);
    CHECK(loaded.sim->map().levelHash() == sim.map().levelHash());
}

TEST_CASE("Stuck: a pawn walking into a wall for 10 s walks through it for 3 s") {
    std::unique_ptr<Sim> owned = earthSim(0.0f);
    Sim& sim = *owned;
    const Entity pawn = earthSpawn(sim, "Human", CellPos{10, 10, 0});
    EARTH_REQUIRE(pawn != kNoEntity);
    EARTH_REQUIRE(earthSpawn(sim, "Wall", CellPos{11, 10, 0}) != kNoEntity);
    entt::registry& reg = sim.things().registry();
    sim.submit(SetWorkPriorityCommand{pawn, "Construction", 0});
    sim.submit(DraftCommand{pawn, true});
    sim.step();
    // A path straight through the wall (as a stale path or a bad wedge would leave it).
    Mover& mover = reg.get<Mover>(pawn);
    mover.path = {CellPos{11, 10, 0}, CellPos{12, 10, 0}};
    mover.next = 0;
    mover.fromX = reg.get<Position>(pawn).x;
    mover.fromY = reg.get<Position>(pawn).y;
    mover.gaveUp = false;
    bool phased = false;
    for (int i = 0; i < 400 && !phased; ++i) {
        // Keep the walk alive: the movement gives up on a walled path, the watchdog is what is tested.
        Mover& m = reg.get<Mover>(pawn);
        if (m.path.empty()) {
            m.path = {CellPos{11, 10, 0}, CellPos{12, 10, 0}};
            m.next = 0;
            m.fromX = reg.get<Position>(pawn).x;
            m.fromY = reg.get<Position>(pawn).y;
        }
        sim.step();
        phased = reg.get<Mover>(pawn).phaseSteps > 0;
    }
    CHECK(phased);
    sim.stepMany(60);
    CHECK(reg.get<Position>(pawn).x > 11.5f);
}

TEST_CASE("Stuck: the unstuck order puts a pawn on the nearest clear spot") {
    std::unique_ptr<Sim> owned = earthSim(0.0f);
    Sim& sim = *owned;
    const Entity pawn = earthSpawn(sim, "Human", CellPos{20, 20, 0});
    EARTH_REQUIRE(pawn != kNoEntity);
    entt::registry& reg = sim.things().registry();
    // Wedged in the middle of a wall block.
    for (int y = 19; y <= 21; ++y) {
        for (int x = 19; x <= 21; ++x) {
            if (x != 20 || y != 20) {
                EARTH_REQUIRE(earthSpawn(sim, "Wall", CellPos{x, y, 0}) != kNoEntity);
            }
        }
    }
    sim.submit(UnstuckCommand{pawn});
    sim.step();
    CHECK(earthLastError(sim).empty());
    const Position& pos = reg.get<Position>(pawn);
    const CellPos at{static_cast<int>(std::floor(pos.x)), static_cast<int>(std::floor(pos.y)), 0};
    CHECK(sim.map().walkable(at));
    CHECK((at.x < 19 || at.x > 21 || at.y < 19 || at.y > 21));
    CHECK(std::abs(at.x - 20) <= 2);
    CHECK(std::abs(at.y - 20) <= 2);
}
