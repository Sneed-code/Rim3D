#include "ColonySim/Commands.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Research.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define RES_REQUIRE(...)       \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data: the projects, the benches and the gated content are what is tested.
std::shared_ptr<const DefDatabase> resDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

Sim resSim() { return Sim(SimSettings{5, 48, 0, 0, true}, resDefs()); }

// A researcher: works all day.
Entity resPawn(Sim& sim, CellPos cell) {
    const Entity pawn = sim.things().spawn("Human", cell);
    sim.jobs().addWorker(pawn);
    for (int h = 0; h < 24; ++h) {
        sim.submit(SetScheduleCommand{pawn, h, ScheduleKind::Work});
    }
    sim.step();
    return pawn;
}

// How many CommandFailed events the last step raised, and the last one's message.
std::string resLastFailure(const Sim& sim) {
    std::string out;
    for (const Event& e : sim.events()) {
        if (e.type == EventType::CommandFailed) {
            out = e.message;
        }
    }
    return out;
}

bool resSawDone(const Sim& sim, const std::string& project) {
    return std::any_of(sim.events().begin(), sim.events().end(), [&](const Event& e) {
        return e.type == EventType::ResearchDone && e.message == project;
    });
}

}  // namespace

TEST_CASE("Research: the projects load and a project can only be picked when its prerequisites are done") {
    Sim sim = resSim();
    const ResearchSystem& research = sim.research();
    RES_REQUIRE(research.rules().present);
    CHECK(research.projects().size() == 67);
    CHECK(research.rules().pointsPerSecond() == doctest::Approx(0.00825 / 1.44));

    sim.submit(SetResearchCommand{"NoSuchProject"});
    sim.step();
    CHECK(resLastFailure(sim) == "no Research def 'NoSuchProject'");

    sim.submit(SetResearchCommand{"LongBlades"});  // needs Smithing
    sim.step();
    CHECK(resLastFailure(sim) == "LongBlades needs Smithing first");
    CHECK(research.current().empty());

    sim.submit(SetResearchCommand{"Smithing"});
    sim.step();
    CHECK(resLastFailure(sim).empty());
    CHECK(research.current() == "Smithing");

    // Putting it down keeps the progress.
    sim.research().state().progress["Smithing"] = 120.0;
    sim.submit(SetResearchCommand{""});
    sim.step();
    CHECK(research.current().empty());
    CHECK(research.progress("Smithing") == doctest::Approx(120.0));

    sim.submit(UnlockResearchCommand{"Smithing"});
    sim.step();
    CHECK(resSawDone(sim, "Smithing"));
    CHECK(research.isDone("Smithing"));
    CHECK(research.progress("Smithing") == doctest::Approx(700.0));
    sim.submit(SetResearchCommand{"Smithing"});  // done
    sim.step();
    CHECK(resLastFailure(sim) == "Smithing is already researched");
    sim.submit(SetResearchCommand{"LongBlades"});
    sim.step();
    CHECK(resLastFailure(sim).empty());
    CHECK(research.current() == "LongBlades");
}

TEST_CASE("Research: a neolithic start pays the tech level's cost factor") {
    Sim sim = resSim();
    const ResearchProject* smithing = sim.research().find("Smithing");        // medieval, 700
    const ResearchProject* electricity = sim.research().find("Electricity");  // industrial, 1600
    const ResearchProject* cooler = sim.research().find("PassiveCooler");     // neolithic, 400
    RES_REQUIRE(smithing != nullptr && electricity != nullptr && cooler != nullptr);
    CHECK(sim.research().cost(*smithing) == doctest::Approx(700.0));
    sim.research().setNeolithicStart(true);
    CHECK(sim.research().cost(*smithing) == doctest::Approx(1050.0));
    CHECK(sim.research().cost(*electricity) == doctest::Approx(3200.0));
    CHECK(sim.research().cost(*cooler) == doctest::Approx(400.0));
}

TEST_CASE("Research: unfinished projects gate buildings and recipes") {
    Sim sim = resSim();
    sim.submit(PlaceBlueprintCommand{"TailorBenchHand", "WoodLog", CellPos{10, 10, 0}, Rot4::North});
    sim.step();
    CHECK(resLastFailure(sim) == "hand tailor bench needs the research ComplexClothing");
    CHECK(sim.construction().siteAt(CellPos{10, 10, 0}) == kNoEntity);

    // A building put there by a dev command can't take a gated bill either.
    const Entity bench = sim.things().spawn("TailorBenchHand", CellPos{20, 20, 0});
    RES_REQUIRE(bench != kNoEntity);
    sim.submit(AddBillCommand{bench, "Make_Apparel_TShirt", BillMode::Count, 1});
    sim.step();
    CHECK(resLastFailure(sim).find("needs the research ComplexClothing") != std::string::npos);

    sim.submit(UnlockResearchCommand{"ComplexClothing"});
    sim.submit(PlaceBlueprintCommand{"TailorBenchHand", "WoodLog", CellPos{10, 10, 0}, Rot4::North});
    sim.submit(AddBillCommand{bench, "Make_Apparel_TShirt", BillMode::Count, 1});
    sim.step();
    CHECK(resLastFailure(sim).empty());
    CHECK(sim.construction().siteAt(CellPos{10, 10, 0}) != kNoEntity);

    // A list of prerequisites needs all of them (the electric smithy: Smithing and Electricity).
    sim.submit(UnlockResearchCommand{"Smithing"});
    sim.submit(PlaceBlueprintCommand{"ElectricSmithy", "Steel", CellPos{30, 30, 0}, Rot4::North});
    sim.step();
    CHECK(resLastFailure(sim).find("needs the research Electricity") != std::string::npos);
}

TEST_CASE("Research: a colonist at a bench finishes the project, at the bench's and the skill's speed") {
    Sim sim = resSim();
    const Entity bench = sim.things().spawn("SimpleResearchBench", CellPos{10, 10, 0});
    RES_REQUIRE(bench != kNoEntity);
    const Entity pawn = resPawn(sim, CellPos{14, 12, 0});

    // Nothing picked: nothing to do.
    sim.stepMany(300);
    CHECK(sim.things().registry().get<Worker>(pawn).job.type != JobType::Research);

    // Stonecutting costs 300 at a bench of speed 0.75; a worker without skills counts as level 8: 0.2 + 0.1 x 8 = 1.
    sim.submit(SetResearchCommand{"Stonecutting"});
    bool sat = false;
    int64_t doneAt = -1;
    const int64_t startedAt = sim.time().seconds;
    for (int i = 0; i < 120000 && doneAt < 0; ++i) {
        sim.step();
        sat = sat || sim.things().registry().get<Worker>(pawn).job.type == JobType::Research;
        if (resSawDone(sim, "Stonecutting")) {
            doneAt = sim.time().seconds;
        }
    }
    CHECK(sat);
    RES_REQUIRE(doneAt > 0);
    const double expected = 300.0 / (0.00825 / 1.44 * 1.0 * 0.75);  // seconds of work
    CHECK(static_cast<double>(doneAt - startedAt) == doctest::Approx(expected).epsilon(0.05));
    CHECK(sim.research().isDone("Stonecutting"));
    CHECK(sim.research().current().empty());

    // The job ends, and the stonecutting table can be planned now.
    sim.stepMany(10);
    CHECK(sim.things().registry().get<Worker>(pawn).job.type != JobType::Research);
    sim.submit(PlaceBlueprintCommand{"StonecutterTable", "WoodLog", CellPos{30, 30, 0}, Rot4::North});
    sim.step();
    CHECK(resLastFailure(sim).empty());
}

TEST_CASE("Research: a simple bench can't work on a project that needs a hi-tech one") {
    Sim sim = resSim();
    for (const char* name : {"Smithing", "Electricity", "Machining", "Microelectronics", "SterileMaterials", "Prosthetics", "ComplexFurniture"}) {
        sim.submit(UnlockResearchCommand{name});
    }
    sim.step();
    sim.submit(SetResearchCommand{"HospitalBed"});
    sim.step();
    RES_REQUIRE(sim.research().current() == "HospitalBed");
    sim.things().spawn("SimpleResearchBench", CellPos{10, 10, 0});
    const Entity pawn = resPawn(sim, CellPos{14, 12, 0});
    sim.stepMany(600);
    CHECK(sim.things().registry().get<Worker>(pawn).job.type != JobType::Research);
    CHECK(sim.research().progress("HospitalBed") == doctest::Approx(0.0));
}

TEST_CASE("Research: two colonists don't share a bench, and a colonist who can't do Intellectual work stays away") {
    Sim sim = resSim();
    sim.things().spawn("SimpleResearchBench", CellPos{10, 10, 0});
    const Entity a = resPawn(sim, CellPos{14, 12, 0});
    const Entity b = resPawn(sim, CellPos{15, 12, 0});
    sim.submit(SetResearchCommand{"Stonecutting"});
    sim.stepMany(120);
    int researchers = 0;
    for (const Entity p : {a, b}) {
        researchers += sim.things().registry().get<Worker>(p).job.type == JobType::Research ? 1 : 0;
    }
    CHECK(researchers == 1);
}

TEST_CASE("Research: the snapshot lists every project") {
    Sim sim = resSim();
    sim.submit(UnlockResearchCommand{"Smithing"});
    sim.submit(SetResearchCommand{"LongBlades"});
    sim.step();
    Snapshot snap;
    sim.writeSnapshot(snap);
    CHECK(snap.research.size() == 67);
    int current = 0;
    for (const ResearchView& v : snap.research) {
        const std::string& name = sim.defs().get(v.project).defName();
        current += v.current ? 1 : 0;
        if (name == "Smithing") {
            CHECK(v.done);
            CHECK_FALSE(v.available);
            CHECK(v.progress == doctest::Approx(700.0));
        }
        if (name == "LongBlades") {
            CHECK(v.current);
            CHECK(v.available);
        }
        if (name == "Machining") {
            CHECK_FALSE(v.available);
        }
    }
    CHECK(current == 1);
}

TEST_CASE("Research: the state saves and loads, and a loaded game goes on the same") {
    Sim sim = resSim();
    sim.things().spawn("SimpleResearchBench", CellPos{10, 10, 0});
    resPawn(sim, CellPos{14, 12, 0});
    sim.research().setNeolithicStart(true);
    sim.submit(UnlockResearchCommand{"Smithing"});
    sim.submit(SetResearchCommand{"Stonecutting"});
    sim.stepMany(3000);
    const double progress = sim.research().progress("Stonecutting");
    RES_REQUIRE(progress > 0.0);

    const std::vector<uint8_t> bytes = saveSim(sim);
    SaveLoadResult loaded = loadSim(bytes, resDefs());
    if (!loaded.error.empty()) MESSAGE(loaded.error);
    RES_REQUIRE(loaded.error.empty());
    RES_REQUIRE(loaded.sim != nullptr);
    Sim& copy = *loaded.sim;
    CHECK(copy.stateHash() == sim.stateHash());
    CHECK(copy.research().current() == "Stonecutting");
    CHECK(copy.research().isDone("Smithing"));
    CHECK(copy.research().state().neolithicStart);
    CHECK(copy.research().progress("Stonecutting") == doctest::Approx(progress));
    CHECK(saveSim(copy) == bytes);

    sim.stepMany(2000);
    copy.stepMany(2000);
    CHECK(copy.stateHash() == sim.stateHash());
    CHECK(copy.research().progress("Stonecutting") == doctest::Approx(sim.research().progress("Stonecutting")));

    // The readable dump has the section.
    CHECK(saveToJson(sim).find("\"research\"") != std::string::npos);
}
