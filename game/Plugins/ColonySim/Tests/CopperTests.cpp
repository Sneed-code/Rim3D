// Copper: an easy, common ore for wiring and components (sneed, 2026-11-15; docs/design/mining.md "Copper").

#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Mineables.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace colony;

#define COPPER_REQUIRE(...)    \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data; with `withCopperOre` false, copper ore never appears at map creation.
std::shared_ptr<const DefDatabase> copperDefs(bool withCopperOre) {
    namespace fs = std::filesystem;
    const fs::path root = fs::path(COLONY_DATA_DIR);
    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(root / "defs")) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    std::vector<DefSource> sources;
    for (const fs::path& file : files) {
        std::ifstream in(file, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        std::string body = text.str();
        if (!withCopperOre && file.filename() == "Rocks.json") {
            const std::string from = "\"dropDef\": \"Copper\", \"lumpSize\": [20, 40], \"commonality\": 1.5";
            const size_t at = body.find(from);
            CHECK(at != std::string::npos);
            if (at != std::string::npos) body.replace(at, from.size(), "\"dropDef\": \"Copper\", \"lumpSize\": [20, 40], \"commonality\": 0");
        }
        sources.push_back(DefSource{file.parent_path().filename().string(), file.filename().string(), body});
    }
    std::ifstream tuningFile(root / "tuning.json", std::ios::binary);
    std::stringstream tuning;
    tuning << tuningFile.rdbuf();
    auto db = std::make_shared<DefDatabase>();
    std::vector<std::string> errors;
    DefDatabase::build(sources, tuning.str(), *db, errors);
    for (const std::string& e : errors) {
        MESSAGE(e);
    }
    return db;
}

std::shared_ptr<const DefDatabase> copperRealDefs() {
    static const std::shared_ptr<const DefDatabase> defs = copperDefs(true);
    return defs;
}

int copperItemCount(Sim& sim, const char* def) {
    int total = 0;
    const DefId id = sim.defs().idOf("Things", def);
    for (const auto [e, thing, stack] : sim.things().registry().view<const Thing, const StackCount>().each()) {
        (void)e;
        total += thing.def == id ? stack.count : 0;
    }
    return total;
}

}  // namespace

TEST_CASE("Copper: maps have copper ore lumps, more than steel, and the other ores stay where they were") {
    SimSettings settings;
    settings.seed = 42;
    settings.mapSize = 256;
    settings.checkpointEvery = 0;
    Sim sim(settings, copperRealDefs());
    Sim noCopper(settings, copperDefs(false));
    const DefId copperOre = sim.defs().idOf("Things", "Ore_Copper");
    const DefId steelOre = sim.defs().idOf("Things", "Ore_Steel");
    COPPER_REQUIRE(copperOre.valid());
    CHECK(sim.mineables().isOre(copperOre));
    CHECK(sim.mineables().maxHitPoints(copperOre) == 1000);
    CHECK(sim.mineables().maxHitPoints(copperOre) < sim.mineables().maxHitPoints(steelOre));
    CHECK(noCopper.mineables().countOf(copperOre) == 0);

    // Over a few seeds copper is the most common ore (1.5 times steel's lumps on average).
    size_t copperCells = 0;
    size_t steelCells = 0;
    for (const uint64_t seed : {42ULL, 1ULL, 7ULL}) {
        settings.seed = seed;
        Sim s(settings, copperRealDefs());
        copperCells += s.mineables().countOf(copperOre);
        steelCells += s.mineables().countOf(steelOre);
    }
    CHECK(copperCells > 0);
    CHECK(copperCells > steelCells);

    // Copper only takes plain rock: every other ore cell is the same as with no copper at all.
    size_t replaced = 0;
    for (int y = 0; y < sim.map().height(); ++y) {
        for (int x = 0; x < sim.map().width(); ++x) {
            const CellPos c{x, y, 0};
            const DefId with = sim.mineables().defAt(c);
            const DefId without = noCopper.mineables().defAt(c);
            if (with == copperOre) {
                ++replaced;
                CHECK_FALSE(noCopper.mineables().isOre(without));
            } else {
                CHECK(with == without);
            }
        }
    }
    CHECK(replaced == sim.mineables().countOf(copperOre));
}

TEST_CASE("Copper: a colonist mines copper ore into a stack of copper, quicker than steel") {
    Sim sim(SimSettings{5, 32, 0, 0, true}, copperRealDefs());
    sim.mineables().place(CellPos{12, 8, 0}, sim.defs().idOf("Things", "Ore_Copper"));
    const Entity pawn = sim.things().spawn("Human", CellPos{8, 8, 0});
    sim.jobs().addWorker(pawn);
    sim.submit(DesignateCommand{CellRect{12, 8, 1, 1, 0}, DesignationKind::Mine});
    int copperSteps = 0;
    for (; copperSteps < 20000 && sim.mineables().count() > 0; ++copperSteps) {
        sim.step();
    }
    COPPER_REQUIRE(sim.mineables().count() == 0);
    const int copper = copperItemCount(sim, "Copper");
    CHECK(copper > 0);

    Sim steel(SimSettings{5, 32, 0, 0, true}, copperRealDefs());
    steel.mineables().place(CellPos{12, 8, 0}, steel.defs().idOf("Things", "Ore_Steel"));
    const Entity miner = steel.things().spawn("Human", CellPos{8, 8, 0});
    steel.jobs().addWorker(miner);
    steel.submit(DesignateCommand{CellRect{12, 8, 1, 1, 0}, DesignationKind::Mine});
    int steelSteps = 0;
    for (; steelSteps < 20000 && steel.mineables().count() > 0; ++steelSteps) {
        steel.step();
    }
    COPPER_REQUIRE(steel.mineables().count() == 0);
    CHECK(copper == copperItemCount(steel, "Steel"));  // the same 40 a cell, before skill
    CHECK(copperSteps < steelSteps);
}

TEST_CASE("Copper: wiring and components use it, and a colony can get it without mining") {
    const DefDatabase& defs = *copperRealDefs();
    const Def* copper = defs.find("Things", "Copper");
    COPPER_REQUIRE(copper != nullptr);
    CHECK(copper->number("stats.marketValue") < defs.find("Things", "Steel")->number("stats.marketValue"));
    CHECK(copper->number("deep.commonality") > 0.0);  // deep drills find it too

    // Power conduits are wired with copper, not steel.
    const Def* conduit = defs.find("Things", "PowerConduit");
    COPPER_REQUIRE(conduit != nullptr);
    CHECK(conduit->number("costs.Copper") == 1.0);
    CHECK_FALSE(conduit->has("costs.Steel"));

    // Components are made at the fabrication bench from 12 steel and 4 copper.
    const Def* recipe = defs.find("Recipes", "MakeComponentIndustrial");
    COPPER_REQUIRE(recipe != nullptr);
    CHECK(recipe->text("researchPrerequisite") == "Fabrication");
    CHECK(recipe->textAt("recipeUsers", 0) == "FabricationBench");
    CHECK(recipe->textAt("ingredients.1.items", 0) == "Copper");
    CHECK(recipe->number("ingredients.1.count") == 4.0);
    CHECK(recipe->number("products.ComponentIndustrial") == 1.0);

    // The long-range mineral scanner can be tuned to copper.
    const Def* scanner = defs.find("Things", "LongRangeMineralScanner");
    COPPER_REQUIRE(scanner != nullptr);
    bool tunable = false;
    for (size_t i = 0; i < scanner->count("deepWork.ores"); ++i) {
        tunable = tunable || scanner->textAt("deepWork.ores", i) == "Ore_Copper";
    }
    CHECK(tunable);
}

TEST_CASE("Copper: both Crashlanded starts bring 150 copper") {
    for (const char* scenario : {"Crashlanded", "CrashlandedRainforest"}) {
        CAPTURE(scenario);
        SimSettings settings;
        settings.seed = 42;
        settings.mapSize = 128;
        settings.checkpointEvery = 0;
        settings.scenario = scenario;
        Sim sim(settings, copperRealDefs());
        CHECK(copperItemCount(sim, "Copper") == 150);
    }
}
