#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/DeepResources.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Mineables.h"
#include "ColonySim/Power.h"
#include "ColonySim/Research.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define MAZ_REQUIRE(...)                    \
    do {                                    \
        const bool mazOk = (__VA_ARGS__);   \
        CHECK(mazOk);                       \
        if (!mazOk) {                       \
            return;                         \
        }                                   \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> mazDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

// A 64 m map under a clear sky with a solar generator at (2,2) and a conduit row and column (x = 9) the machines stand beside.
std::unique_ptr<Sim> mazSim() {
    auto owned = std::make_unique<Sim>(SimSettings{5, 64, 0, 0, true}, mazDefs());
    Sim& sim = *owned;
    sim.setOutdoorOverride(20.0f);
    sim.submit(SetWeatherCommand{"Clear", 100000.0f});
    sim.power().setSun([](const GameTime&) { return 1.0; });
    sim.things().spawn("SolarGenerator", CellPos{2, 2, 0});
    for (int x = 6; x <= 9; ++x) {
        sim.things().spawn("PowerConduit", CellPos{x, 5, 0});
    }
    for (int y = 6; y <= 40; ++y) {
        sim.things().spawn("PowerConduit", CellPos{9, y, 0});
    }
    return owned;
}

Entity mazWorker(Sim& sim, CellPos cell) {
    const Entity pawn = sim.things().spawn("Human", cell);
    sim.jobs().addWorker(pawn);
    Colonist colonist;
    colonist.skills.push_back(SkillRecord{"Intellectual", 8, Passion::None, 0.0f, false});
    sim.things().registry().emplace<Colonist>(pawn, colonist);
    for (int h = 0; h < 24; ++h) {
        sim.submit(SetScheduleCommand{pawn, h, ScheduleKind::Work});
    }
    sim.step();
    return pawn;
}

std::string mazLastFailure(const Sim& sim) {
    std::string out;
    for (const Event& e : sim.events()) {
        if (e.type == EventType::CommandFailed) {
            out = e.message;
        }
    }
    return out;
}

}  // namespace

TEST_CASE("MultiAnalyzer: the project unlocks the analyzer, with the wiki's cost and range") {
    auto owner = mazSim();
    Sim& sim = *owner;
    CHECK_FALSE(sim.research().unlocked(sim.defs().get(sim.defs().idOf("Things", "MultiAnalyzer"))));
    for (const char* name : {"Electricity", "Machining", "Microelectronics", "MultiAnalyzer"}) {
        sim.submit(UnlockResearchCommand{name});
    }
    sim.step();
    CHECK(sim.research().unlocked(sim.defs().get(sim.defs().idOf("Things", "MultiAnalyzer"))));
    const Def& def = sim.defs().get(sim.defs().idOf("Things", "MultiAnalyzer"));
    CHECK(def.integer("costs.Steel") == 40);
    CHECK(def.integer("costs.Plasteel") == 50);
    CHECK(def.integer("costs.Gold") == 20);
    CHECK(def.integer("costs.ComponentIndustrial") == 8);
    CHECK(def.number("power.consumption") == doctest::Approx(200.0));
    CHECK(def.number("building.facilityRange") == doctest::Approx(8.0));
}

TEST_CASE("MultiAnalyzer: a powered analyzer within 8 cells links a hi-tech bench, an unpowered or far one does not") {
    auto owner = mazSim();
    Sim& sim = *owner;
    const Entity bench = sim.things().spawn("HiTechResearchBench", CellPos{10, 10, 0});
    const Entity near = sim.things().spawn("MultiAnalyzer", CellPos{10, 13, 0});
    const Entity far = sim.things().spawn("MultiAnalyzer", CellPos{10, 30, 0});
    MAZ_REQUIRE(bench != kNoEntity && near != kNoEntity && far != kNoEntity);
    sim.stepMany(5);
    CHECK(sim.jobs().benchLinkedTo(bench, "MultiAnalyzer"));
    sim.things().destroy(near);
    sim.stepMany(5);
    CHECK_FALSE(sim.jobs().benchLinkedTo(bench, "MultiAnalyzer"));  // only the far one is left, and the far one is out of range

    // With no power network the near analyzer does nothing either.
    Sim dark(SimSettings{5, 64, 0, 0, true}, mazDefs());
    const Entity darkBench = dark.things().spawn("HiTechResearchBench", CellPos{10, 10, 0});
    dark.things().spawn("MultiAnalyzer", CellPos{10, 13, 0});
    dark.stepMany(5);
    CHECK_FALSE(dark.jobs().benchLinkedTo(darkBench, "MultiAnalyzer"));
}

TEST_CASE("MultiAnalyzer: a project that needs the analyzer waits for one, and a linked bench is 10% faster") {
    auto owner = mazSim();
    Sim& sim = *owner;
    for (const char* name : {"Electricity", "Machining", "Microelectronics", "MultiAnalyzer", "Smithing"}) {
        sim.submit(UnlockResearchCommand{name});
    }
    sim.step();
    const Entity bench = sim.things().spawn("HiTechResearchBench", CellPos{10, 10, 0});
    MAZ_REQUIRE(bench != kNoEntity);
    mazWorker(sim, CellPos{14, 14, 0});
    sim.submit(SetResearchCommand{"Fabrication"});  // requiredFacility MultiAnalyzer
    sim.stepMany(900);
    CHECK(sim.research().progress("Fabrication") == doctest::Approx(0.0));  // no analyzer: nobody works it

    sim.things().spawn("MultiAnalyzer", CellPos{10, 13, 0});
    sim.stepMany(60);  // the worker walks over
    const double start = sim.research().progress("Fabrication");
    sim.stepMany(600);
    const double gained = sim.research().progress("Fabrication") - start;
    MAZ_REQUIRE(gained > 0.0);
    // Level 8 counts as 100%; the hi-tech bench is 1.0 and the analyzer adds 0.1.
    const double perSecond = sim.research().rules().pointsPerSecond() * 1.1;
    CHECK(gained == doctest::Approx(perSecond * 600.0).epsilon(0.05));
}

TEST_CASE("MineralScanner: a worked scanner puts a lump of its tuned ore 6 to 25 cells away") {
    auto owner = mazSim();
    Sim& sim = *owner;
    const Entity scanner = sim.things().spawn("LongRangeMineralScanner", CellPos{10, 10, 0});
    MAZ_REQUIRE(scanner != kNoEntity);
    sim.step();
    const CellPos origin = sim.things().registry().get<Footprint>(scanner).origin;
    const DefId gold = sim.defs().idOf("Things", "Ore_Gold");
    MAZ_REQUIRE(gold.valid());

    // Tuning: a bad ore and a thing that is not a scanner are refused, gold is accepted and kept.
    sim.submit(ScannerCommand{scanner, "Ore_Granite"});
    sim.step();
    CHECK_FALSE(mazLastFailure(sim).empty());
    sim.submit(ScannerCommand{sim.things().spawn("PowerConduit", CellPos{30, 30, 0}), "Ore_Gold"});
    sim.step();
    CHECK_FALSE(mazLastFailure(sim).empty());
    sim.submit(ScannerCommand{scanner, "Ore_Gold"});
    sim.step();
    CHECK(mazLastFailure(sim).empty());
    CHECK(sim.deepResources().tuneOf(origin) == gold);

    // Nearly a guaranteed find's worth of work is done: one researcher's few seconds finish it.
    DeepResourcesSaved s = sim.deepResources().saved();
    for (DeepWork& w : s.work) {
        if (w.at == origin) {
            w.work = 691200.0 - 3.0;
        }
    }
    MAZ_REQUIRE(sim.deepResources().restore(s));
    const size_t rockBefore = sim.mineables().countOf(gold);
    mazWorker(sim, CellPos{14, 14, 0});
    Event found;
    bool seen = false;
    for (int i = 0; i < 600 && !seen; ++i) {
        sim.step();
        for (const Event& e : sim.events()) {
            if (e.type == EventType::OreLumpFound) {
                found = e;
                seen = true;
            }
        }
    }
    MAZ_REQUIRE(seen);
    CHECK(found.def == gold);
    CHECK(found.count >= 8);
    CHECK(found.count <= 12);
    CHECK(sim.mineables().countOf(gold) == rockBefore + static_cast<size_t>(found.count));
    CHECK(sim.mineables().defAt(found.cell) == gold);
    const double dx = found.cell.x - (origin.x + 1);
    const double dy = found.cell.y - (origin.y + 1);
    const double dist = std::sqrt(dx * dx + dy * dy);
    CHECK(dist >= 5.0);
    CHECK(dist <= 27.0);
    // The tuning outlives the find, and the work started again.
    CHECK(sim.deepResources().tuneOf(origin) == gold);
    CHECK(sim.deepResources().workAt(origin) < 100.0);
    // Mining a lump cell gives gold.
    CHECK(sim.mineables().dropOf(found.cell).def == sim.defs().idOf("Things", "Gold"));
}

TEST_CASE("MineralScanner: not under a roof, the tuning and the lump survive a save and load") {
    auto owner = mazSim();
    Sim& sim = *owner;
    const Entity scanner = sim.things().spawn("LongRangeMineralScanner", CellPos{10, 10, 0});
    MAZ_REQUIRE(scanner != kNoEntity);
    sim.step();
    const CellPos origin = sim.things().registry().get<Footprint>(scanner).origin;
    sim.submit(ScannerCommand{scanner, "Ore_Uranium"});
    sim.step();
    const DefId uranium = sim.defs().idOf("Things", "Ore_Uranium");
    CHECK(sim.deepResources().tuneOf(origin) == uranium);

    const_cast<Map&>(sim.map()).setThickRoof(CellPos{origin.x + 1, origin.y + 1, 0}, true);
    std::string why;
    CHECK_FALSE(sim.jobs().deepWorkable(scanner, why));
    CHECK(why == "the scanner is under a roof");

    const std::vector<uint8_t> bytes = saveSim(sim);
    SaveLoadResult loaded = loadSim(bytes, mazDefs());
    if (!loaded.error.empty()) MESSAGE(loaded.error);
    MAZ_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->deepResources().tuneOf(origin) == uranium);
    CHECK(loaded.sim->stateHash() == sim.stateHash());
    CHECK(saveSim(*loaded.sim) == bytes);
}

TEST_CASE("MineralScanner: the extra ores are never in the rock at map creation") {
    auto owner = mazSim();
    for (const char* name : {"Ore_Plasteel", "Ore_Gold", "Ore_Uranium", "Ore_Jade"}) {
        CHECK(owner->mineables().countOf(owner->defs().idOf("Things", name)) == 0);
    }
}
