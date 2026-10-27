#include "ColonySim/Defs.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Weather.h"
#include "ColonySim/World.h"

#include <doctest.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// W2: the sim owns a world (docs/design/world-map.md). The start tile gives the colony's map its
// biome, climate and seed; the world goes in saves and replays.

// Tests build without exceptions, so a failed must-have check ends the test case by returning.
#define WORLD_SIM_REQUIRE(cond) \
    do {                        \
        CHECK(cond);            \
        if (!(cond)) {          \
            return;             \
        }                       \
    } while (false)

namespace {

std::shared_ptr<const DefDatabase> worldSimDefs() {
    static std::shared_ptr<const DefDatabase> db;
    if (db == nullptr) {
        auto loaded = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *loaded, errors);
        db = loaded;
    }
    return db;
}

SimSettings worldSimSettings(uint64_t seed, int startTile = -1) {
    SimSettings s;
    s.seed = seed;
    s.mapSize = 96;
    s.world.enabled = true;
    s.world.startTile = startTile;
    return s;
}

// A tile a colony can start on in `biome`, on the world of `seed`.
int worldSimTileIn(uint64_t seed, const std::string& biome) {
    const World w = World::generate(*worldSimDefs(), seed);
    StartCriteria c;
    c.biome = biome;
    return w.suggestStartTile(c);
}

}  // namespace

TEST_CASE("World sim: without a world the sim is as before, in the temperate forest") {
    SimSettings s;
    s.seed = 42;
    s.mapSize = 96;
    Sim sim(s, worldSimDefs());
    CHECK(sim.world() == nullptr);
    CHECK(sim.site() == nullptr);
    CHECK(sim.biome() == "TemperateForest");
    CHECK(sim.setupErrors().empty());
}

TEST_CASE("World sim: the scenario's biome picks the start tile, and the settings keep it") {
    SimSettings s = worldSimSettings(42);
    s.scenario = "Crashlanded";
    Sim sim(s, worldSimDefs());
    WORLD_SIM_REQUIRE(sim.setupErrors().empty());
    WORLD_SIM_REQUIRE(sim.world() != nullptr);
    WORLD_SIM_REQUIRE(sim.site() != nullptr);
    const World& world = *sim.world();
    CHECK(sim.settings().world.seed == 42);  // 0 took the sim's seed
    CHECK(sim.settings().world.width == world.width());
    CHECK(sim.settings().world.startTile == world.startTile());
    CHECK(world.startTile() >= 0);
    CHECK(world.canStartAt(world.startTile()));
    CHECK(sim.site()->tile == world.startTile());
    CHECK(sim.site()->biome == "TemperateForest");
    CHECK(sim.biome() == "TemperateForest");
    CHECK(sim.site()->mapSeed == world.siteFor(world.startTile()).mapSeed);
}

TEST_CASE("World sim: an arid shrubland tile makes an arid colony at the tile's own temperature") {
    const int tile = worldSimTileIn(42, "AridShrubland");
    WORLD_SIM_REQUIRE(tile >= 0);
    Sim sim(worldSimSettings(42, tile), worldSimDefs());
    WORLD_SIM_REQUIRE(sim.setupErrors().empty());
    CHECK(sim.biome() == "AridShrubland");
    CHECK(sim.world()->startTile() == tile);
    // The biome's seasons, moved so they average to the tile's yearly mean.
    CHECK(std::fabs(sim.weather().yearMeanTemperature() - static_cast<double>(sim.site()->temperature)) < 0.01);
    CHECK(sim.weather().seasonalMean(0) != sim.weather().seasonalMean(kSecondsPerDay * 15 * 3));  // still has seasons
}

TEST_CASE("World sim: the start tile seeds the map, whatever the sim's seed") {
    const int forest = worldSimTileIn(42, "TemperateForest");
    const int desert = worldSimTileIn(42, "AridShrubland");
    WORLD_SIM_REQUIRE(forest >= 0);
    WORLD_SIM_REQUIRE(desert >= 0);
    SimSettings a = worldSimSettings(42, forest);
    SimSettings b = worldSimSettings(42, forest);
    b.seed = 7;
    b.world.seed = 42;  // the same world, another sim seed
    SimSettings c = worldSimSettings(42, desert);
    const Sim simA(a, worldSimDefs());
    const Sim simB(b, worldSimDefs());
    const Sim simC(c, worldSimDefs());
    CHECK(simA.map().contentHash() == simB.map().contentHash());
    CHECK(simA.map().contentHash() != simC.map().contentHash());
    CHECK(simA.world()->hash() == simB.world()->hash());
}

TEST_CASE("World sim: a tile a colony can't start on is a setup error") {
    const World w = World::generate(*worldSimDefs(), 42);
    int sea = -1;
    for (int i = 0; i < w.tileCount() && sea < 0; ++i) {
        if (w.tile(i).water) {
            sea = i;
        }
    }
    WORLD_SIM_REQUIRE(sea >= 0);
    Sim sim(worldSimSettings(42, sea), worldSimDefs());
    WORLD_SIM_REQUIRE(sim.setupErrors().size() == 1);
    CHECK(sim.setupErrors()[0].find("can't start a colony") != std::string::npos);
    CHECK(sim.site() == nullptr);
    CHECK(sim.biome() == "TemperateForest");
}

TEST_CASE("World sim: a save keeps the world and its start tile, and loads to the same game") {
    SimSettings s = worldSimSettings(42, worldSimTileIn(42, "BorealForest"));
    s.scenario = "Crashlanded";
    Sim original(s, worldSimDefs());
    WORLD_SIM_REQUIRE(original.setupErrors().empty());
    original.stepMany(600);
    const std::vector<uint8_t> bytes = saveSim(original);
    SaveLoadResult loaded = loadSim(bytes, worldSimDefs());
    WORLD_SIM_REQUIRE(loaded.sim != nullptr);
    Sim& copy = *loaded.sim;
    WORLD_SIM_REQUIRE(copy.world() != nullptr);
    CHECK(copy.world()->hash() == original.world()->hash());
    CHECK(copy.world()->startTile() == original.world()->startTile());
    CHECK(copy.biome() == "BorealForest");
    CHECK(copy.stateHash() == original.stateHash());
    original.stepMany(600);
    copy.stepMany(600);
    CHECK(copy.stateHash() == original.stateHash());

    std::string json;
    std::string error;
    WORLD_SIM_REQUIRE(saveFileToJson(bytes, json, error));
    CHECK(json.find("\"world\"") != std::string::npos);
}

TEST_CASE("World sim: a save made without a world loads without one") {
    SimSettings s;
    s.seed = 42;
    s.mapSize = 96;
    Sim original(s, worldSimDefs());
    SaveLoadResult loaded = loadSim(saveSim(original), worldSimDefs());
    WORLD_SIM_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->world() == nullptr);
    CHECK(loaded.sim->stateHash() == original.stateHash());
}

TEST_CASE("World sim: a replay keeps the world settings") {
    SimSettings s = worldSimSettings(42);  // a smaller world, its start tile picked
    s.world.width = 90;
    s.world.height = 45;
    Sim sim(s, worldSimDefs());
    WORLD_SIM_REQUIRE(sim.setupErrors().empty());
    sim.stepMany(10);
    Replay read;
    std::string error;
    WORLD_SIM_REQUIRE(readReplay(writeReplay(recordReplay(sim)), read, error));
    CHECK(read.settings.world.enabled);
    CHECK(read.settings.world.seed == 42);
    CHECK(read.settings.world.width == 90);
    CHECK(read.settings.world.height == 45);
    CHECK(read.settings.world.startTile >= 0);
    CHECK(read.settings.world.startTile == sim.world()->startTile());

    Replay none;
    SimSettings plain;
    plain.mapSize = 32;
    Sim plainSim(plain, worldSimDefs());
    WORLD_SIM_REQUIRE(readReplay(writeReplay(recordReplay(plainSim)), none, error));
    CHECK_FALSE(none.settings.world.enabled);

    Replay bad;
    CHECK_FALSE(readReplay(R"({"format": "rim3d-replay", "version": 1, "settings": {"seed": 1, "mapSize": 32,
        "world": {"width": 5000}}})", bad, error));
    CHECK(error.find("settings.world") != std::string::npos);
}
