// Floors laid by colonists (docs/design/construction.md "Floors"): a floor plan is planned with
// PlaceBlueprintCommand naming a Terrain floor, delivered to and worked like a building, and lays
// the floor when done.

#include "ColonySim/Commands.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Floors.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Research.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <memory>
#include <string>
#include <vector>

using namespace colony;

#define FLOORPLAN_REQUIRE(...) \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> floorPlanDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

Sim floorPlanSim() { return Sim(SimSettings{5, 48, 0, 0, true}, floorPlanDefs()); }

void floorPlanSpawn(Sim& sim, const std::string& def, CellPos cell, int count = 1) {
    sim.submit(SpawnThingCommand{def, cell, count});
    sim.step();
}

// Plans a floor on one cell; returns the error, empty when it was planned.
std::string floorPlanPlace(Sim& sim, const std::string& floor, const std::string& stuff, CellPos cell) {
    sim.submit(PlaceBlueprintCommand{floor, stuff, cell, Rot4::North});
    sim.step();
    for (const Event& e : sim.events()) {
        if (e.type == EventType::CommandFailed) {
            return e.message.empty() ? std::string("failed") : e.message;
        }
    }
    return {};
}

int floorPlanItems(const Sim& sim, const std::string& def) {
    const DefId id = sim.defs().idOf("Things", def);
    int total = 0;
    sim.things().registry().view<const Thing, const StackCount>().each(
        [&](const Thing& thing, const StackCount& stack) { total += thing.def == id ? stack.count : 0; });
    return total;
}

bool floorPlanAllLaid(const Sim& sim, int x0, int y0, int size, DefId floor) {
    for (int y = y0; y < y0 + size; ++y) {
        for (int x = x0; x < x0 + size; ++x) {
            if (sim.floors().at(CellPos{x, y, 0}) != floor) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

TEST_CASE("Floor plans: a colonist lays a 3 x 3 wood floor, the middle cell too, from the wood brought") {
    Sim sim = floorPlanSim();
    floorPlanSpawn(sim, "Human", CellPos{10, 10, 0});
    floorPlanSpawn(sim, "WoodLog", CellPos{6, 6, 0}, 40);
    const DefId wood = sim.defs().idOf("Terrain", "WoodFloor");
    for (int y = 20; y < 23; ++y) {
        for (int x = 20; x < 23; ++x) {
            CHECK(floorPlanPlace(sim, "WoodFloor", "", CellPos{x, y, 0}).empty());
        }
    }
    const Entity middle = sim.construction().siteAt(CellPos{21, 21, 0});
    FLOORPLAN_REQUIRE(middle != kNoEntity);
    CHECK(sim.construction().isFloorPlan(middle));
    CHECK(sim.defs().get(sim.things().registry().get<Thing>(middle).def).defName() == "FloorPlan");
    const ConstructionSite& site = sim.things().registry().get<ConstructionSite>(middle);
    CHECK(site.floor == wood);
    FLOORPLAN_REQUIRE(site.materials.size() == 1);
    CHECK(site.materials[0].needed == 3);  // the wood floor's 3 logs
    CHECK(site.workTotal > 0.0f);
    // The plan blocks nothing: the cell is walked on as before.
    CHECK(sim.map().walkable(CellPos{21, 21, 0}));

    // The snapshot names the floor it lays.
    Snapshot snap;
    sim.writeSnapshot(snap);
    const ThingView* view = snap.find(middle);
    FLOORPLAN_REQUIRE(view != nullptr);
    CHECK(view->floor == wood);
    CHECK(view->materialsNeeded == 3);

    int steps = 0;
    while (steps < 6000 && !floorPlanAllLaid(sim, 20, 20, 3, wood)) {
        sim.step();
        ++steps;
    }
    CHECK(floorPlanAllLaid(sim, 20, 20, 3, wood));
    CHECK(sim.construction().siteAt(CellPos{21, 21, 0}) == kNoEntity);
    CHECK(floorPlanItems(sim, "WoodLog") == 40 - 27);
    CHECK(sim.map().buildingAt(CellPos{21, 21, 0}) == kNoEntity);
    CHECK(sim.stats().failedJobs == 0);
}

TEST_CASE("Floor plans: where a floor can and can't be planned") {
    Sim sim = floorPlanSim();
    // Not twice on one cell, and not where that floor already lies.
    CHECK(floorPlanPlace(sim, "WoodFloor", "", CellPos{5, 5, 0}).empty());
    CHECK(!floorPlanPlace(sim, "WoodFloor", "", CellPos{5, 5, 0}).empty());
    sim.submit(PlaceFloorCommand{CellRect{8, 8, 1, 1, 0}, "WoodFloor"});
    sim.step();
    CHECK(!floorPlanPlace(sim, "WoodFloor", "", CellPos{8, 8, 0}).empty());
    // Not under a wall (one planned there, or one standing).
    sim.submit(PlaceBlueprintCommand{"Wall", "WoodLog", CellPos{12, 12, 0}, Rot4::North});
    sim.step();
    CHECK(!floorPlanPlace(sim, "WoodFloor", "", CellPos{12, 12, 0}).empty());
    // A stuff floor needs a stuff that fits, and its research.
    CHECK(!floorPlanPlace(sim, "StoneTile", "", CellPos{15, 15, 0}).empty());
    const std::string locked = floorPlanPlace(sim, "StoneTile", "BlocksGranite", CellPos{15, 15, 0});
    if (!sim.research().unlocked(sim.defs().get(sim.defs().idOf("Terrain", "StoneTile")))) {
        CHECK(!locked.empty());
        sim.submit(UnlockResearchCommand{"Stonecutting"});
        sim.step();
        CHECK(floorPlanPlace(sim, "StoneTile", "BlocksGranite", CellPos{15, 15, 0}).empty());
    }
    const Entity tile = sim.construction().siteAt(CellPos{15, 15, 0});
    FLOORPLAN_REQUIRE(tile != kNoEntity);
    const ConstructionSite& site = sim.things().registry().get<ConstructionSite>(tile);
    FLOORPLAN_REQUIRE(site.materials.size() == 1);
    CHECK(site.materials[0].item == sim.defs().idOf("Things", "BlocksGranite"));
    CHECK(site.materials[0].needed == 4);
    // A name that is neither a building nor a floor.
    CHECK(!floorPlanPlace(sim, "NoSuchFloor", "", CellPos{20, 20, 0}).empty());
}

TEST_CASE("Floor plans: cancelling gives back what was brought") {
    Sim sim = floorPlanSim();
    floorPlanSpawn(sim, "Human", CellPos{10, 10, 0});
    floorPlanSpawn(sim, "WoodLog", CellPos{6, 6, 0}, 10);
    FLOORPLAN_REQUIRE(floorPlanPlace(sim, "WoodFloor", "", CellPos{20, 20, 0}).empty());
    const Entity plan = sim.construction().siteAt(CellPos{20, 20, 0});
    int steps = 0;
    while (steps < 3000 && sim.things().registry().get<ConstructionSite>(plan).materialsHave() < 3) {
        sim.step();
        ++steps;
    }
    FLOORPLAN_REQUIRE(sim.things().registry().get<ConstructionSite>(plan).materialsHave() == 3);
    sim.submit(CancelBuildCommand{plan});
    sim.step();
    CHECK(sim.construction().siteAt(CellPos{20, 20, 0}) == kNoEntity);
    CHECK(!sim.floors().at(CellPos{20, 20, 0}).valid());
    CHECK(floorPlanItems(sim, "WoodLog") == 10);
}

TEST_CASE("Floor plans: a save in the middle of laying floors loads back the same game") {
    SimSettings settings{9, 48, 0, 0, true};
    Sim original(settings, floorPlanDefs());
    original.submit(SpawnThingCommand{"Human", CellPos{10, 10, 0}});
    original.submit(SpawnThingCommand{"WoodLog", CellPos{6, 6, 0}, 30});
    original.step();
    for (int x = 20; x < 24; ++x) {
        original.submit(PlaceBlueprintCommand{"WoodFloor", "", CellPos{x, 20, 0}, Rot4::North});
    }
    original.step();
    bool live = false;
    for (int i = 0; i < 3000 && !live; ++i) {
        original.step();
        original.things().registry().view<const ConstructionSite>().each([&](const ConstructionSite& site) {
            live = live || (site.floor.valid() && site.stage == ConstructionStage::Frame);
        });
    }
    FLOORPLAN_REQUIRE(live);
    const std::vector<uint8_t> bytes = saveSim(original);
    SaveLoadResult loaded = loadSim(bytes, floorPlanDefs());
    FLOORPLAN_REQUIRE(loaded.error.empty());
    FLOORPLAN_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == original.stateHash());
    CHECK(saveSim(*loaded.sim) == bytes);
    CHECK(saveToJson(original).find("\"WoodFloor\"") != std::string::npos);
    original.stepMany(2000);
    loaded.sim->stepMany(2000);
    CHECK(loaded.sim->stateHash() == original.stateHash());
    const DefId wood = original.defs().idOf("Terrain", "WoodFloor");
    CHECK(original.floors().at(CellPos{23, 20, 0}) == wood);
    CHECK(loaded.sim->floors().at(CellPos{23, 20, 0}) == wood);
}
