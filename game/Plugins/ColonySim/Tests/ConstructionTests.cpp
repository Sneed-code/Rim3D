#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Regions.h"
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
#define BUILD_REQUIRE(...)     \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data: the buildings in Buildings.json and their costs are part of what is tested.
std::shared_ptr<const DefDatabase> buildDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

Sim buildSim() { return Sim(SimSettings{5, 48, 0, 0, true}, buildDefs()); }

Entity buildSpawn(Sim& sim, const std::string& def, CellPos cell, int count = 1) {
    sim.submit(SpawnThingCommand{def, cell, count});
    sim.step();
    for (const Event& e : sim.events()) {
        if (e.type == EventType::ThingSpawned && e.cell == cell && sim.defs().get(e.def).defName() == def) {
            return e.thing;
        }
    }
    return kNoEntity;
}

// Places a blueprint by command; returns the site, or kNoEntity with the failure in `error`.
Entity buildPlace(Sim& sim, const std::string& def, const std::string& stuff, CellPos cell, Rot4 rot = Rot4::North,
                  std::string* error = nullptr) {
    sim.submit(PlaceBlueprintCommand{def, stuff, cell, rot});
    sim.step();
    for (const Event& e : sim.events()) {
        if (e.type == EventType::CommandFailed && error != nullptr) {
            *error = e.message;
        }
    }
    return sim.construction().siteAt(cell);
}

int buildCount(const Sim& sim, EventType type) {
    return static_cast<int>(std::count_if(sim.events().begin(), sim.events().end(),
                                          [type](const Event& e) { return e.type == type; }));
}

// Steps until `done` says so, at most `limit` steps; returns the steps it took, or -1.
template <typename F>
int buildRunUntil(Sim& sim, int limit, F done) {
    for (int i = 0; i < limit; ++i) {
        if (done()) {
            return i;
        }
        sim.step();
    }
    return done() ? limit : -1;
}

// All of one item def: lying on the map, plus carried and delivered.
int buildItemTotal(const Sim& sim, const std::string& def) {
    const DefId id = sim.defs().idOf("Things", def);
    int total = 0;
    sim.things().registry().view<const Thing, const StackCount>().each(
        [&](const Thing& thing, const StackCount& stack) { total += thing.def == id ? stack.count : 0; });
    return total;
}

int buildItemsOnMap(const Sim& sim, const std::string& def) {
    const DefId id = sim.defs().idOf("Things", def);
    int total = 0;
    sim.things().registry().view<const Thing, const StackCount, const Position>().each(
        [&](const Thing& thing, const StackCount& stack, const Position&) { total += thing.def == id ? stack.count : 0; });
    return total;
}

bool buildIsWall(const Sim& sim, CellPos cell) { return sim.map().passage(cell) == Passage::Blocked; }

const ConstructionSite* buildSite(const Sim& sim, Entity e) {
    return sim.things().registry().valid(e) ? sim.things().registry().try_get<ConstructionSite>(e) : nullptr;
}

}  // namespace

TEST_CASE("Construction: the defs give RimWorld's costs, work and hit points") {
    Sim sim = buildSim();
    const Construction& c = sim.construction();
    const DefDatabase& defs = sim.defs();
    const DefId steel = defs.idOf("Things", "Steel");
    const DefId wood = defs.idOf("Things", "WoodLog");
    const DefId wall = defs.idOf("Things", "Wall");
    const DefId stove = defs.idOf("Things", "FueledStove");
    BUILD_REQUIRE(steel.valid() && wood.valid() && wall.valid() && stove.valid());

    CHECK(c.buildable(wall));
    CHECK_FALSE(c.buildable(steel));
    CHECK_FALSE(c.buildable(defs.idOf("Things", "Human")));

    // A steel wall: 5 steel, 135 ticks of work (x 1.44 game seconds), 300 hit points.
    const auto steelWall = c.costOf(wall, steel);
    BUILD_REQUIRE(steelWall.size() == 1);
    CHECK(steelWall[0].item == steel);
    CHECK(steelWall[0].count == 5);
    CHECK(c.workToBuild(wall, steel) == doctest::Approx(194.4));
    CHECK(c.maxHitPoints(wall, steel) == 300);
    // Wood: 195 hit points and 95 ticks (the wiki's numbers).
    CHECK(c.maxHitPoints(wall, wood) == 195);
    CHECK(c.workToBuild(wall, wood) / 1.44f == doctest::Approx(94.5).epsilon(0.01));

    // The stove takes no stuff: 80 steel, 2000 ticks.
    const auto stoveCost = c.costOf(stove, DefId{});
    BUILD_REQUIRE(stoveCost.size() == 1);
    CHECK(stoveCost[0].count == 80);
    CHECK(c.workToBuild(stove, DefId{}) == doctest::Approx(2880.0));
    CHECK(c.maxHitPoints(stove, DefId{}) == 180);

    CHECK(c.stuffFits(wall, steel));
    CHECK(c.stuffFits(wall, wood));
    CHECK_FALSE(c.stuffFits(wall, DefId{}));
    CHECK_FALSE(c.stuffFits(wall, defs.idOf("Things", "Silver")));  // silver is no stuff here
    CHECK_FALSE(c.stuffFits(stove, steel));                         // the stove takes none
    CHECK(c.stuffFits(stove, DefId{}));
}

TEST_CASE("Construction: every building in the data can be planned with a stuff that fits") {
    Sim sim = buildSim();
    const DefDatabase& defs = sim.defs();
    const DefId steel = defs.idOf("Things", "Steel");
    int buildings = 0;
    for (const DefId id : defs.idsOfKind("Things")) {
        if (defs.get(id).text("category") != "Building" || !defs.get(id).has("designationCategory")) {
            continue;  // buildings the map is made with (ruin walls, geysers) are never planned
        }
        ++buildings;
        DefId stuff = defs.get(id).count("stuffCategories") > 0 ? steel : DefId{};
        if (stuff.valid() && !sim.construction().stuffFits(id, stuff)) {
            stuff = defs.idOf("Things", "Cloth");  // sandbags are fabric or leather
        }
        CHECK_MESSAGE(sim.construction().buildable(id), defs.get(id).defName());
        CHECK_MESSAGE(sim.construction().stuffFits(id, stuff), defs.get(id).defName());
        CHECK_MESSAGE(!sim.construction().costOf(id, stuff).empty(), defs.get(id).defName());
        CHECK_MESSAGE(sim.construction().workToBuild(id, stuff) > 0.0f, defs.get(id).defName());
    }
    CHECK(buildings >= 8);  // wall, door, bed, stool, three tables, stove
}

TEST_CASE("Construction: a blueprint is planned where a building fits and blocks nothing") {
    Sim sim = buildSim();
    std::string error;
    const Entity site = buildPlace(sim, "Wall", "Steel", CellPos{10, 10, 0});
    BUILD_REQUIRE(site != kNoEntity);
    CHECK(buildCount(sim, EventType::CommandFailed) == 0);
    const ConstructionSite* s = buildSite(sim, site);
    BUILD_REQUIRE(s != nullptr);
    CHECK(s->stage == ConstructionStage::Blueprint);
    CHECK(s->materials.size() == 1);
    CHECK(s->materials[0].needed == 5);
    CHECK(s->materials[0].have == 0);
    CHECK(sim.things().registry().get<Thing>(site).category == ThingCategory::Building);
    // Not on the building layer: pawns walk through and the regions don't see it.
    CHECK(sim.map().buildingAt(CellPos{10, 10, 0}) == kNoEntity);
    CHECK(sim.map().walkable(CellPos{10, 10, 0}));
    CHECK_FALSE(sim.things().registry().all_of<Footprint>(site));

    // Overlap, bad def, bad stuff, off the map.
    buildPlace(sim, "Wall", "Steel", CellPos{10, 10, 0}, Rot4::North, &error);
    CHECK(error.find("already planned") != std::string::npos);
    buildPlace(sim, "Wall", "WoodLog", CellPos{-1, 3, 0}, Rot4::North, &error);
    CHECK(error.find("can't build") != std::string::npos);
    buildPlace(sim, "NoSuchThing", "", CellPos{3, 3, 0}, Rot4::North, &error);
    CHECK(error.find("no Things def") != std::string::npos);
    buildPlace(sim, "Wall", "", CellPos{3, 3, 0}, Rot4::North, &error);
    CHECK(error.find("can't be made of") != std::string::npos);
    buildPlace(sim, "FueledStove", "Steel", CellPos{3, 3, 0}, Rot4::North, &error);
    CHECK(error.find("not made of anything") != std::string::npos);
    buildPlace(sim, "Steel", "", CellPos{3, 3, 0}, Rot4::North, &error);
    CHECK(error.find("can't be built") != std::string::npos);
    buildSpawn(sim, "Wall", CellPos{20, 20, 0});
    buildPlace(sim, "Wall", "Steel", CellPos{20, 20, 0}, Rot4::North, &error);
    CHECK(error.find("there is a building") != std::string::npos);

    // A bed overlapping the first blueprint with its second cell fails, and leaves no plan behind.
    buildPlace(sim, "Bed", "WoodLog", CellPos{10, 9, 0}, Rot4::North, &error);
    CHECK(error.find("already planned") != std::string::npos);
    CHECK(sim.construction().siteAt(CellPos{10, 9, 0}) == kNoEntity);

    // East turns a 3x1 stove into 1x3: it covers three cells of a column.
    const Entity stove = buildPlace(sim, "FueledStove", "", CellPos{30, 30, 0}, Rot4::East);
    BUILD_REQUIRE(stove != kNoEntity);
    CHECK(sim.construction().siteAt(CellPos{30, 32, 0}) == stove);
    CHECK(sim.construction().siteAt(CellPos{31, 30, 0}) == kNoEntity);
}

TEST_CASE("Construction: a pawn fetches steel, delivers it and builds a wall that blocks the way") {
    Sim sim = buildSim();
    const Entity pawn = buildSpawn(sim, "Human", CellPos{10, 10, 0});
    buildSpawn(sim, "Steel", CellPos{5, 5, 0}, 10);
    const Entity site = buildPlace(sim, "Wall", "Steel", CellPos{14, 10, 0});
    BUILD_REQUIRE(site != kNoEntity);

    bool sawFrame = false;
    bool sawBuild = false;
    const int steps = buildRunUntil(sim, 1200, [&] {
        sawFrame = sawFrame || buildCount(sim, EventType::FrameStarted) > 0;
        for (const Event& e : sim.events()) {
            sawBuild = sawBuild || (e.type == EventType::PawnAction && e.action == PawnAction::Build);
        }
        return buildIsWall(sim, CellPos{14, 10, 0});
    });
    BUILD_REQUIRE(steps >= 0);
    // The work alone is 194.4 / 1.7 = 114 seconds at construction speed 1.
    CHECK(steps > 114);
    CHECK(sawFrame);
    CHECK(sawBuild);
    CHECK(sim.construction().siteAt(CellPos{14, 10, 0}) == kNoEntity);
    const Entity wall = sim.map().buildingAt(CellPos{14, 10, 0});
    BUILD_REQUIRE(wall != kNoEntity);
    CHECK(sim.things().registry().get<HitPoints>(wall).current == 300);
    CHECK(sim.things().registry().get<BuiltOf>(wall).stuff == sim.defs().idOf("Things", "Steel"));
    // 5 of the 10 steel went into the wall; the other 5 are still lying where they were.
    CHECK(buildItemTotal(sim, "Steel") == 5);
    CHECK(sim.stats().failedJobs == 0);
    sim.stepMany(10);
    CHECK(sim.things().registry().get<Worker>(pawn).job.type != JobType::BuildFrame);
}

TEST_CASE("Construction: wood walls get wood's hit points, a stove needs two trips and takes no stuff") {
    Sim sim = buildSim();
    buildSpawn(sim, "Human", CellPos{10, 10, 0});
    buildSpawn(sim, "WoodLog", CellPos{4, 4, 0}, 5);
    buildSpawn(sim, "Steel", CellPos{6, 6, 0}, 75);
    buildSpawn(sim, "Steel", CellPos{6, 7, 0}, 30);
    const Entity wallSite = buildPlace(sim, "Wall", "WoodLog", CellPos{12, 12, 0});
    const Entity stoveSite = buildPlace(sim, "FueledStove", "", CellPos{20, 20, 0}, Rot4::East);
    BUILD_REQUIRE(wallSite != kNoEntity);
    BUILD_REQUIRE(stoveSite != kNoEntity);

    BUILD_REQUIRE(buildRunUntil(sim, 6000, [&] {
                      return buildIsWall(sim, CellPos{12, 12, 0}) && sim.map().buildingAt(CellPos{20, 21, 0}) != kNoEntity;
                  }) >= 0);
    const Entity wall = sim.map().buildingAt(CellPos{12, 12, 0});
    CHECK(sim.things().registry().get<HitPoints>(wall).max == 195);
    const Entity stove = sim.map().buildingAt(CellPos{20, 22, 0});
    BUILD_REQUIRE(stove != kNoEntity);
    CHECK(sim.map().buildingAt(CellPos{20, 20, 0}) == stove);
    CHECK_FALSE(sim.things().registry().all_of<BuiltOf>(stove));
    CHECK(sim.things().registry().get<HitPoints>(stove).max == 180);
    CHECK(buildItemTotal(sim, "Steel") == 25);
    CHECK(buildItemTotal(sim, "WoodLog") == 0);
    CHECK(sim.stats().failedJobs == 0);
}

TEST_CASE("Construction: with no materials nothing happens, and the plan waits for them") {
    Sim sim = buildSim();
    buildSpawn(sim, "Human", CellPos{10, 10, 0});
    const Entity site = buildPlace(sim, "Wall", "Steel", CellPos{14, 10, 0});
    sim.stepMany(400);
    const ConstructionSite* s = buildSite(sim, site);
    BUILD_REQUIRE(s != nullptr);
    CHECK(s->stage == ConstructionStage::Blueprint);
    buildSpawn(sim, "Steel", CellPos{3, 3, 0}, 5);
    CHECK(buildRunUntil(sim, 1500, [&] { return buildIsWall(sim, CellPos{14, 10, 0}); }) >= 0);
}

TEST_CASE("Construction: Construction priority 0 means the pawn doesn't build") {
    Sim sim = buildSim();
    const Entity pawn = buildSpawn(sim, "Human", CellPos{10, 10, 0});
    buildSpawn(sim, "Steel", CellPos{5, 5, 0}, 5);
    sim.submit(SetWorkPriorityCommand{pawn, "Construction", 0});
    sim.submit(SetWorkPriorityCommand{pawn, "Hauling", 0});
    buildPlace(sim, "Wall", "Steel", CellPos{14, 10, 0});
    sim.stepMany(600);
    CHECK_FALSE(buildIsWall(sim, CellPos{14, 10, 0}));
    sim.submit(SetWorkPriorityCommand{pawn, "Construction", 1});
    CHECK(buildRunUntil(sim, 1500, [&] { return buildIsWall(sim, CellPos{14, 10, 0}); }) >= 0);
}

TEST_CASE("Construction: two builders on a row of walls never fetch the same steel twice") {
    Sim sim = buildSim();
    buildSpawn(sim, "Human", CellPos{10, 10, 0});
    buildSpawn(sim, "Human", CellPos{10, 12, 0});
    buildSpawn(sim, "Steel", CellPos{5, 5, 0}, 75);
    buildSpawn(sim, "Steel", CellPos{5, 6, 0}, 75);
    for (int x = 20; x < 26; ++x) {
        BUILD_REQUIRE(buildPlace(sim, "Wall", "Steel", CellPos{x, 14, 0}) != kNoEntity);
    }
    BUILD_REQUIRE(buildRunUntil(sim, 6000, [&] {
                      for (const Event& e : sim.events()) {
                          if (e.type == EventType::JobEnded && e.message != "done") {
                              MESSAGE("t=" << sim.time().seconds << " " << std::string(jobTypeName(e.job)) << " " << e.message);
                          }
                      }
                      for (int x = 20; x < 26; ++x) {
                          if (!buildIsWall(sim, CellPos{x, 14, 0})) {
                              return false;
                          }
                      }
                      return true;
                  }) >= 0);
    CHECK(buildItemTotal(sim, "Steel") == 150 - 30);
    CHECK(sim.stats().failedJobs == 0);
    CHECK(sim.construction().siteAt(CellPos{22, 14, 0}) == kNoEntity);
}

TEST_CASE("Construction: a better skill builds faster") {
    const auto timeFor = [](int level) {
        Sim sim = buildSim();
        const Entity pawn = buildSpawn(sim, "Human", CellPos{12, 11, 0});
        Colonist colonist;
        colonist.skills.push_back(SkillRecord{"Construction", level, Passion::None, 0.0f, false});
        sim.things().registry().emplace<Colonist>(pawn, std::move(colonist));
        buildSpawn(sim, "Steel", CellPos{12, 12, 0}, 5);
        buildPlace(sim, "Wall", "Steel", CellPos{14, 10, 0});
        return buildRunUntil(sim, 6000, [&] { return buildIsWall(sim, CellPos{14, 10, 0}); });
    };
    const int skilled = timeFor(20);
    const int unskilled = timeFor(0);
    BUILD_REQUIRE(skilled > 0);
    BUILD_REQUIRE(unskilled > 0);
    CHECK(unskilled > 3 * skilled);  // 0.3 against 2.05 of construction speed on the work, walking aside
}

TEST_CASE("Construction: cancelling a frame gives back what was delivered") {
    Sim sim = buildSim();
    buildSpawn(sim, "Human", CellPos{10, 10, 0});
    buildSpawn(sim, "Steel", CellPos{5, 5, 0}, 75);
    buildSpawn(sim, "Steel", CellPos{5, 6, 0}, 5);
    const Entity site = buildPlace(sim, "FueledStove", "", CellPos{20, 20, 0});
    BUILD_REQUIRE(site != kNoEntity);
    BUILD_REQUIRE(buildRunUntil(sim, 3000, [&] {
                      const ConstructionSite* s = buildSite(sim, site);
                      return s != nullptr && s->stage == ConstructionStage::Frame && s->materialsHave() >= 75;
                  }) >= 0);
    sim.submit(CancelBuildCommand{site});
    sim.step();
    CHECK(buildCount(sim, EventType::CommandFailed) == 0);
    CHECK(buildSite(sim, site) == nullptr);
    CHECK(sim.construction().siteAt(CellPos{20, 20, 0}) == kNoEntity);
    sim.stepMany(5);
    // Nothing was lost: the steel lies on the ground again, or is in the pawn's hands.
    CHECK(buildItemTotal(sim, "Steel") == 80);
    CHECK(sim.stats().failedJobs <= 1);  // a delivery under way can fail when the plan goes
    // And cancelling again says so.
    sim.submit(CancelBuildCommand{site});
    sim.step();
    CHECK(buildCount(sim, EventType::CommandFailed) == 1);
}

TEST_CASE("Construction: deconstructing returns about half the materials and opens the way") {
    Sim sim = buildSim();
    buildSpawn(sim, "Human", CellPos{10, 10, 0});
    buildSpawn(sim, "Steel", CellPos{5, 5, 0}, 5);
    buildPlace(sim, "Wall", "Steel", CellPos{14, 10, 0});
    BUILD_REQUIRE(buildRunUntil(sim, 3000, [&] { return buildIsWall(sim, CellPos{14, 10, 0}); }) >= 0);
    const Entity wall = sim.map().buildingAt(CellPos{14, 10, 0});
    CHECK(buildItemTotal(sim, "Steel") == 0);

    sim.submit(DeconstructCommand{wall});
    sim.step();
    CHECK(buildCount(sim, EventType::CommandFailed) == 0);
    CHECK(sim.things().registry().all_of<MarkedForDeconstruct>(wall));
    // Marking twice fails; the snapshot shows the mark.
    sim.submit(DeconstructCommand{wall});
    sim.step();
    CHECK(buildCount(sim, EventType::CommandFailed) == 1);
    Snapshot snap;
    sim.writeSnapshot(snap);
    const ThingView* view = snap.find(wall);
    BUILD_REQUIRE(view != nullptr);
    CHECK(view->markedForDeconstruct);
    CHECK(view->stage == ConstructionStage::Built);

    BUILD_REQUIRE(buildRunUntil(sim, 3000, [&] { return !sim.things().exists(wall); }) >= 0);
    CHECK_FALSE(buildIsWall(sim, CellPos{14, 10, 0}));
    const int back = buildItemsOnMap(sim, "Steel");
    CHECK((back == 2 || back == 3));  // 5 x 0.5, rounded at random as RimWorld does
    CHECK(sim.stats().failedJobs == 0);
}

TEST_CASE("Construction: cancelling the mark keeps the building, and a pawn working on it stops") {
    Sim sim = buildSim();
    buildSpawn(sim, "Human", CellPos{10, 10, 0});
    const Entity door = buildSpawn(sim, "Door", CellPos{14, 10, 0});
    BUILD_REQUIRE(door != kNoEntity);
    sim.submit(DeconstructCommand{door});
    sim.stepMany(30);
    sim.submit(CancelBuildCommand{door});
    sim.stepMany(400);
    CHECK(sim.things().exists(door));
    CHECK_FALSE(sim.things().registry().all_of<MarkedForDeconstruct>(door));
}

TEST_CASE("Construction: a pawn standing where a wall is finished is moved out, and items are moved off") {
    Sim sim = buildSim();
    const Entity builder = buildSpawn(sim, "Human", CellPos{10, 10, 0});
    const Entity bystander = buildSpawn(sim, "Human", CellPos{16, 16, 0});
    sim.submit(SetWorkPriorityCommand{bystander, "Construction", 0});
    sim.submit(SetWorkPriorityCommand{bystander, "Hauling", 0});
    (void)builder;
    buildSpawn(sim, "Steel", CellPos{5, 5, 0}, 5);
    const Entity wood = buildSpawn(sim, "WoodLog", CellPos{16, 16, 0}, 20);
    buildPlace(sim, "Wall", "Steel", CellPos{16, 16, 0});
    BUILD_REQUIRE(buildRunUntil(sim, 3000, [&] { return buildIsWall(sim, CellPos{16, 16, 0}); }) >= 0);
    const Position& pos = sim.things().registry().get<Position>(bystander);
    CHECK(cellOf(pos) != CellPos{16, 16, 0});
    CHECK(sim.map().walkable(cellOf(pos)));
    // The wood is still on the map, not under the wall.
    CHECK(buildItemsOnMap(sim, "WoodLog") == 20);
    CHECK(sim.things().itemsAt(CellPos{16, 16, 0}).empty());
    // It is the same stack, moved: a pawn on its way to it (or a job holding it) still has it.
    CHECK(sim.things().spawned(wood));
    CHECK(sim.things().registry().get<StackCount>(wood).count == 20);
}

TEST_CASE("Construction: a pawn walking when a wall goes up under it gives up the walk, so its job asks again") {
    Sim sim = buildSim();
    const Entity walker = buildSpawn(sim, "Human", CellPos{16, 16, 0});
    sim.submit(SetWorkPriorityCommand{walker, "Construction", 0});
    sim.submit(SetWorkPriorityCommand{walker, "Hauling", 0});
    const Entity site = buildPlace(sim, "Wall", "Steel", CellPos{16, 16, 0});
    BUILD_REQUIRE(site != kNoEntity);
    Mover& mover = sim.things().registry().get<Mover>(walker);
    const Position& at = sim.things().registry().get<Position>(walker);
    mover.walkTo(at.x, at.y, {CellPos{30, 16, 0}});
    CHECK_FALSE(mover.arrived());
    CHECK(sim.construction().deliver(site, sim.defs().idOf("Things", "Steel"), 5) == 5);
    const Entity wall = sim.construction().complete(site);
    BUILD_REQUIRE(wall != kNoEntity);
    CHECK(cellOf(sim.things().registry().get<Position>(walker)) != CellPos{16, 16, 0});
    CHECK(mover.arrived());  // the route is cleared...
    CHECK(mover.gaveUp);     // ...and the walk is said to be given up, not finished
}

TEST_CASE("Construction: a plan walled in where no pawn can stand is left alone") {
    Sim sim = buildSim();
    buildSpawn(sim, "Human", CellPos{10, 10, 0});
    buildSpawn(sim, "Steel", CellPos{5, 5, 0}, 5);
    for (int y = 19; y <= 21; ++y) {
        for (int x = 19; x <= 21; ++x) {
            if (x != 20 || y != 20) {
                buildSpawn(sim, "Wall", CellPos{x, y, 0});
            }
        }
    }
    buildPlace(sim, "Wall", "Steel", CellPos{20, 20, 0});
    sim.stepMany(600);
    const ConstructionSite* s = buildSite(sim, sim.construction().siteAt(CellPos{20, 20, 0}));
    BUILD_REQUIRE(s != nullptr);
    CHECK(s->stage == ConstructionStage::Blueprint);
    CHECK(sim.stats().failedJobs == 0);
}

TEST_CASE("Construction: a finished building clears the plants under it") {
    Sim sim = buildSim();
    buildSpawn(sim, "Human", CellPos{10, 10, 0});
    buildSpawn(sim, "Steel", CellPos{12, 11, 0}, 5);
    const CellPos under{14, 10, 0};
    const CellPos beside{15, 10, 0};
    const DefId grass = sim.defs().idOf("Things", "Plant_Grass");
    sim.plants().remove(under);
    sim.plants().remove(beside);
    BUILD_REQUIRE(sim.plants().place(under, grass, 1.0f));
    BUILD_REQUIRE(sim.plants().place(beside, grass, 1.0f));
    buildPlace(sim, "Wall", "Steel", under);
    CHECK(sim.plants().has(under));  // a blueprint leaves it alone
    BUILD_REQUIRE(buildRunUntil(sim, 3000, [&] { return sim.map().buildingAt(under) != kNoEntity; }) >= 0);
    CHECK_FALSE(sim.plants().has(under));
    CHECK(sim.plants().has(beside));
}

TEST_CASE("Construction: a door and a bed are built like a wall") {
    Sim sim = buildSim();
    buildSpawn(sim, "Human", CellPos{10, 10, 0});
    buildSpawn(sim, "Steel", CellPos{5, 5, 0}, 75);
    buildSpawn(sim, "WoodLog", CellPos{5, 7, 0}, 50);
    buildPlace(sim, "Door", "Steel", CellPos{14, 10, 0});
    buildPlace(sim, "Bed", "WoodLog", CellPos{30, 30, 0}, Rot4::West);
    BUILD_REQUIRE(buildRunUntil(sim, 9000, [&] {
                      return sim.map().passage(CellPos{14, 10, 0}) == Passage::Door && sim.map().buildingAt(CellPos{31, 30, 0}) != kNoEntity;
                  }) >= 0);
    CHECK(sim.map().walkable(CellPos{14, 10, 0}));
    const Entity bed = sim.map().buildingAt(CellPos{30, 30, 0});
    CHECK(bed == sim.map().buildingAt(CellPos{31, 30, 0}));  // facing West, 2 cells wide
    CHECK(sim.things().registry().all_of<Bed>(bed));          // a built bed can be slept in (needs.md)
    CHECK(buildItemTotal(sim, "Steel") == 50);
    CHECK(buildItemTotal(sim, "WoodLog") == 5);
}

TEST_CASE("Construction: sites show in the snapshot with their progress") {
    Sim sim = buildSim();
    buildSpawn(sim, "Human", CellPos{10, 10, 0});
    buildSpawn(sim, "Steel", CellPos{12, 11, 0}, 5);
    const Entity site = buildPlace(sim, "Wall", "Steel", CellPos{14, 10, 0});
    Snapshot snap;
    sim.writeSnapshot(snap);
    const ThingView* view = snap.find(site);
    BUILD_REQUIRE(view != nullptr);
    CHECK(view->stage == ConstructionStage::Blueprint);
    CHECK(view->category == ThingCategory::Building);
    CHECK(view->materialsNeeded == 5);
    CHECK(view->materialsHave == 0);
    CHECK(view->stuff == sim.defs().idOf("Things", "Steel"));
    CHECK(sim.defs().get(view->def).defName() == "Wall");

    BUILD_REQUIRE(buildRunUntil(sim, 3000, [&] {
                      const ConstructionSite* s = buildSite(sim, site);
                      return s != nullptr && s->stage == ConstructionStage::Frame && s->workDone > 40.0f;
                  }) >= 0);
    sim.writeSnapshot(snap);
    view = snap.find(site);
    BUILD_REQUIRE(view != nullptr);
    CHECK(view->stage == ConstructionStage::Frame);
    CHECK(view->materialsHave == 5);
    CHECK(view->buildProgress > 0.1f);
    CHECK(view->buildProgress < 1.0f);
}

TEST_CASE("Construction: the same commands give the same game, and a save in the middle of building loads back the same") {
    const auto session = [](Sim& sim, int from, int to) {
        for (int i = from; i < to; ++i) {
            if (i == 0) {
                sim.submit(SpawnThingCommand{"Human", CellPos{10, 10, 0}});
                sim.submit(SpawnThingCommand{"Human", CellPos{12, 10, 0}});
                sim.submit(SpawnThingCommand{"Steel", CellPos{5, 5, 0}, 75});
                sim.submit(SpawnThingCommand{"Steel", CellPos{5, 6, 0}, 75});
                sim.submit(SpawnThingCommand{"WoodLog", CellPos{6, 5, 0}, 60});
            }
            if (i == 2) {
                for (int x = 20; x < 25; ++x) {
                    sim.submit(PlaceBlueprintCommand{"Wall", "Steel", CellPos{x, 14, 0}, Rot4::North});
                }
                sim.submit(PlaceBlueprintCommand{"FueledStove", "", CellPos{30, 20, 0}, Rot4::South});
                sim.submit(PlaceBlueprintCommand{"Bed", "WoodLog", CellPos{34, 30, 0}, Rot4::East});
                sim.submit(PlaceBlueprintCommand{"Door", "WoodLog", CellPos{26, 14, 0}, Rot4::North});
            }
            if (i == 900) {
                sim.submit(DeconstructCommand{sim.map().buildingAt(CellPos{20, 14, 0})});
                sim.submit(CancelBuildCommand{sim.construction().siteAt(CellPos{34, 30, 0})});
            }
            if (i == 1400) {
                sim.submit(PlaceBlueprintCommand{"Table2x2", "WoodLog", CellPos{40, 40, 0}, Rot4::North});
                sim.submit(PlaceBlueprintCommand{"Wall", "Steel", CellPos{20, 14, 0}, Rot4::North});
            }
            sim.step();
        }
    };
    SimSettings settings{9, 64, 0, 0, true};
    settings.checkpointEvery = 300;

    Sim straight(settings, buildDefs());
    session(straight, 0, 3000);
    Sim again(settings, buildDefs());
    session(again, 0, 3000);
    CHECK(straight.stateHash() == again.stateHash());

    // Save while a frame has materials and a pawn is mid-job.
    Sim original(settings, buildDefs());
    int saveAt = 0;
    bool sitesLive = false;
    while (!sitesLive && saveAt < 2000) {
        saveAt += 10;
        session(original, saveAt - 10, saveAt);
        original.things().registry().view<const ConstructionSite>().each([&](const ConstructionSite& site) {
            sitesLive = sitesLive || (site.materialsHave() > 0 && site.stage == ConstructionStage::Frame);
        });
    }
    BUILD_REQUIRE(sitesLive);
    const std::vector<uint8_t> bytes = saveSim(original);
    SaveLoadResult loaded = loadSim(bytes, buildDefs());
    BUILD_REQUIRE(loaded.error.empty());
    BUILD_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->stateHash() == original.stateHash());
    CHECK(saveSim(*loaded.sim) == bytes);
    session(original, saveAt, 3000);
    session(*loaded.sim, saveAt, 3000);
    CHECK(loaded.sim->stateHash() == original.stateHash());
    CHECK(original.stateHash() == straight.stateHash());
    CHECK(saveSim(*loaded.sim) == saveSim(original));
    // The readable dump shows the plans.
    CHECK(saveToJson(original).find("\"materials\"") != std::string::npos);

    // And it all got built.
    CHECK(buildIsWall(straight, CellPos{24, 14, 0}));
    CHECK(straight.map().passage(CellPos{26, 14, 0}) == Passage::Door);
    CHECK(straight.stats().failedJobs < 10);
}

TEST_CASE("Construction: a builder holds still while it builds, and walks free again after") {
    Sim sim = buildSim();
    const Entity builder = buildSpawn(sim, "Human", CellPos{10, 10, 0});
    buildSpawn(sim, "Steel", CellPos{12, 10, 0}, 5);
    const Entity site = buildPlace(sim, "Wall", "Steel", CellPos{14, 10, 0});
    BUILD_REQUIRE(site != kNoEntity);
    const Worker& worker = sim.things().registry().get<Worker>(builder);
    const Mover& mover = sim.things().registry().get<Mover>(builder);
    bool heldWhileWorking = false;
    bool heldWhileWalking = false;
    for (int i = 0; i < 3000 && !buildIsWall(sim, CellPos{14, 10, 0}); ++i) {
        sim.step();
        const bool working = worker.job.type == JobType::BuildFrame && worker.job.step >= 1;
        heldWhileWorking = heldWhileWorking || (working && mover.hold);
        heldWhileWalking = heldWhileWalking || (!working && mover.hold);
    }
    CHECK(buildIsWall(sim, CellPos{14, 10, 0}));
    CHECK(heldWhileWorking);
    CHECK_FALSE(heldWhileWalking);
}
