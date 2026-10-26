#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Pathfinder.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"
#include "ColonySim/Zones.h"

#include "TestDefs.h"

#include <doctest.h>

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define JOBS_REQUIRE(...)      \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// A flat 32 m map with the test defs and no hauling test.
Sim jobsTestSim() { return Sim(SimSettings{5, 32, 0, 0, true}, testDefs()); }

// Spawns a thing by command and returns it (the step that applies the command also runs jobs).
Entity jobsSpawn(Sim& sim, const std::string& def, CellPos cell, int count = 1) {
    sim.submit(SpawnThingCommand{def, cell, count});
    sim.step();
    for (const Event& e : sim.events()) {
        if (e.type == EventType::ThingSpawned && e.cell == cell && sim.defs().get(e.def).defName() == def) {
            return e.thing;
        }
    }
    return kNoEntity;
}

int jobsCount(const Sim& sim, EventType type) {
    return static_cast<int>(std::count_if(sim.events().begin(), sim.events().end(),
                                          [type](const Event& e) { return e.type == type; }));
}

// Steps until `done` says so, at most `limit` steps. Returns whether it did.
template <typename F>
bool jobsRunUntil(Sim& sim, int limit, F done) {
    for (int i = 0; i < limit; ++i) {
        if (done()) {
            return true;
        }
        sim.step();
    }
    return done();
}

const Job& jobOf(const Sim& sim, Entity pawn) { return sim.things().registry().get<Worker>(pawn).job; }

void jobsWallRing(Sim& sim, int cx, int cy) {
    for (int y = cy - 1; y <= cy + 1; ++y) {
        for (int x = cx - 1; x <= cx + 1; ++x) {
            if (x != cx || y != cy) {
                sim.submit(SpawnThingCommand{"Wall", CellPos{x, y, 0}});
            }
        }
    }
    sim.step();
}

}  // namespace

TEST_CASE("Jobs: work types come from the data in Work tab order, and new workers do all of them at 3") {
    auto defs = std::make_shared<DefDatabase>();
    std::vector<std::string> errors;
    JOBS_REQUIRE(DefDatabase::loadDirectory(COLONY_DATA_DIR, *defs, errors));
    Sim sim(SimSettings{5, 32, 0, 0, true}, defs);
    const auto& types = sim.jobs().workTypes();
    JOBS_REQUIRE(types.size() == 20);
    CHECK(types.front().defName == "Firefighter");
    CHECK(types.back().defName == "Research");
    for (size_t i = 1; i < types.size(); ++i) {
        CHECK(types[i - 1].naturalPriority >= types[i].naturalPriority);
    }
    const int hauling = sim.jobs().workTypeIndex(defs->idOf("WorkTypes", "Hauling"));
    JOBS_REQUIRE(hauling >= 0);
    CHECK(types[static_cast<size_t>(hauling)].givers.size() == 2);  // HaulToStockpile and Strip (C15); Refuel comes later

    const Entity pawn = jobsSpawn(sim, "Human", CellPos{5, 5, 0});
    JOBS_REQUIRE(sim.things().registry().all_of<Worker, WorkSettings>(pawn));
    const WorkSettings& work = sim.things().registry().get<WorkSettings>(pawn);
    JOBS_REQUIRE(work.entries.size() == 20);
    for (size_t i = 0; i < types.size(); ++i) {
        CHECK(work.entries[i].workType == types[i].def);
        CHECK(work.entries[i].priority == 3);
        CHECK_FALSE(work.entries[i].disabled);
    }
}

TEST_CASE("Jobs: SetWorkPriority sets the Work tab, and a pawn with hauling off doesn't haul") {
    Sim sim = jobsTestSim();
    sim.submit(CreateStockpileCommand{CellRect{2, 2, 3, 3, 0}});
    const Entity pawn = jobsSpawn(sim, "Human", CellPos{10, 10, 0});
    const Entity wall = jobsSpawn(sim, "Wall", CellPos{28, 28, 0});
    sim.submit(SetWorkPriorityCommand{pawn, "Hauling", 0});
    sim.step();
    CHECK(jobsCount(sim, EventType::CommandFailed) == 0);
    jobsSpawn(sim, "Steel", CellPos{20, 20, 0}, 10);
    sim.stepMany(600);
    CHECK(sim.itemsLeft() == 1);
    CHECK(jobOf(sim, pawn).type == JobType::Wait);

    sim.submit(SetWorkPriorityCommand{pawn, "Hauling", 2});
    sim.stepMany(1200);
    CHECK(sim.itemsLeft() == 0);
    CHECK(sim.stats().hauls == 1);

    // What can't be done is refused with a reason.
    sim.submit(SetWorkPriorityCommand{pawn, "Hauling", 5});
    sim.submit(SetWorkPriorityCommand{pawn, "NoSuchWork", 1});
    sim.submit(SetWorkPriorityCommand{wall, "Hauling", 1});
    WorkSettings& work = sim.things().registry().get<WorkSettings>(pawn);
    work.entries[0].disabled = true;  // as a backstory will (C8): Hauling is first in the test defs
    sim.submit(SetWorkPriorityCommand{pawn, "Hauling", 1});
    sim.submit(SetWorkPriorityCommand{pawn, "Hauling", 0});  // switching off disabled work is fine
    sim.step();
    int failed = 0;
    for (const Event& e : sim.events()) {
        if (e.type == EventType::CommandFailed) {
            ++failed;
            CHECK_FALSE(e.message.empty());
        }
    }
    CHECK(failed == 4);
    CHECK(work.entries[0].priority == 0);
}

TEST_CASE("Jobs: the nearest item goes first, and items nobody can reach are skipped without failing") {
    Sim sim = jobsTestSim();
    sim.submit(CreateStockpileCommand{CellRect{1, 1, 2, 2, 0}});
    jobsWallRing(sim, 20, 10);
    const Entity walledIn = jobsSpawn(sim, "Steel", CellPos{20, 10, 0});
    const Entity far = jobsSpawn(sim, "Steel", CellPos{26, 26, 0});
    const Entity near = jobsSpawn(sim, "Steel", CellPos{9, 5, 0});
    const Entity pawn = jobsSpawn(sim, "Human", CellPos{5, 5, 0});
    CHECK(jobOf(sim, pawn).type == JobType::HaulToCell);
    CHECK(jobOf(sim, pawn).thing == near);
    CHECK(jobOf(sim, pawn).workType == sim.defs().idOf("WorkTypes", "Hauling"));
    CHECK(jobsRunUntil(sim, 3 * 3600, [&] { return sim.itemsLeft() == 1; }));
    CHECK(sim.things().spawned(walledIn));
    CHECK_FALSE(sim.things().spawned(far));  // carried to the stockpile and put on the first one's stack
    CHECK(sim.stats().failedJobs == 0);
    CHECK(sim.pathfinder().stats().refused == 0);  // the walled-in item was never even asked for
    CHECK(sim.stats().hauls == 2);
}

TEST_CASE("Jobs: two pawns never hold the same item or stockpile cell") {
    Sim sim(SimSettings{17, 64, 0, 0, true}, testDefs());
    sim.submit(CreateStockpileCommand{CellRect{30, 30, 4, 4, 0}});
    for (int i = 0; i < 24; ++i) {
        sim.submit(SpawnThingCommand{"Steel", CellPos{5 + (i * 7) % 50, 5 + (i * 13) % 50, 0}, 1 + i % 3});
    }
    for (int i = 0; i < 4; ++i) {
        sim.submit(SpawnThingCommand{"Human", CellPos{31 + i, 29, 0}});
    }
    bool clash = false;
    const bool done = jobsRunUntil(sim, 4 * 3600, [&] {
        std::set<uint32_t> things;
        std::set<std::tuple<int, int, int>> cells;
        sim.things().registry().view<const Worker>().each([&](const Worker& worker) {
            if (worker.job.type == JobType::HaulToCell) {
                clash = clash || !things.insert(entt::to_integral(worker.job.thing)).second;
                clash = clash || !cells.insert({worker.job.cell.x, worker.job.cell.y, worker.job.cell.level}).second;
            }
        });
        return sim.itemsLeft() == 0;
    });
    CHECK(done);
    CHECK_FALSE(clash);
    CHECK(sim.stats().failedJobs == 0);
}

TEST_CASE("Jobs: stacks go together up to the stack limit, and the part that doesn't fit stays") {
    Sim sim = jobsTestSim();
    sim.submit(CreateStockpileCommand{CellRect{2, 2, 1, 1, 0}});  // one cell: room for 75 steel
    jobsSpawn(sim, "Steel", CellPos{10, 10, 0}, 50);
    jobsSpawn(sim, "Steel", CellPos{12, 10, 0}, 50);
    jobsSpawn(sim, "Human", CellPos{11, 12, 0});
    CHECK(jobsRunUntil(sim, 3600, [&] { return sim.stats().hauls == 2; }));
    sim.stepMany(600);
    const std::vector<Entity>& stored = sim.things().itemsAt(CellPos{2, 2, 0});
    JOBS_REQUIRE(stored.size() == 1);
    CHECK(sim.things().registry().get<StackCount>(stored[0]).count == 75);
    // The rest: 25 steel where the second stack lay, not hauled since there is no room.
    CHECK(sim.itemsLeft() == 1);
    int outside = 0;
    for (const auto [item, stack] : sim.things().registry().view<const StackCount>().each()) {
        if (sim.jobs().storedPriority(item) == StoragePriority::Unstored) {
            outside += stack.count;
        }
    }
    CHECK(outside == 25);
    CHECK(sim.stats().failedJobs == 0);
}

TEST_CASE("Jobs: better stockpiles pull items from worse ones, and a filter keeps items out") {
    Sim sim = jobsTestSim();
    sim.submit(CreateStockpileCommand{CellRect{2, 2, 2, 2, 0}, StoragePriority::Low});  // zone 1
    const Entity steel = jobsSpawn(sim, "Steel", CellPos{2, 2, 0}, 10);
    const Entity pawn = jobsSpawn(sim, "Human", CellPos{8, 8, 0});
    CHECK(sim.itemsLeft() == 0);  // stored where it lies, in a Low stockpile
    sim.submit(CreateStockpileCommand{CellRect{20, 20, 2, 2, 0}, StoragePriority::Preferred});  // zone 2
    sim.submit(SetStockpileAllowedCommand{2, "Steel", false});
    sim.stepMany(1200);
    CHECK(sim.stats().hauls == 0);  // the better stockpile doesn't take steel
    CHECK(jobOf(sim, pawn).type == JobType::Wait);

    sim.submit(SetStockpileAllowedCommand{2, "Steel", true});
    CHECK(jobsRunUntil(sim, 3600, [&] { return sim.jobs().storedPriority(steel) == StoragePriority::Preferred; }));
    CHECK(sim.zones().zoneAt(cellOf(sim.things().registry().get<Position>(steel))) == 2);

    // Bad zone commands fail with a reason.
    sim.submit(SetStockpileAllowedCommand{9, "Steel", true});
    sim.submit(SetStockpileAllowedCommand{2, "NoSuchThing", true});
    sim.submit(SetStockpilePriorityCommand{9, StoragePriority::Low});
    sim.submit(SetStockpilePriorityCommand{2, StoragePriority::Unstored});
    sim.submit(CreateStockpileCommand{CellRect{20, 20, 2, 2, 0}});  // every cell is in zone 2 already
    sim.submit(RemoveZoneCellsCommand{CellRect{10, 10, 3, 3, 0}});  // no zone there
    sim.step();
    CHECK(jobsCount(sim, EventType::CommandFailed) == 6);
}

TEST_CASE("Jobs: a hauler whose stockpile goes away carries on to another, or puts the item down") {
    for (const bool another : {true, false}) {
        Sim sim = jobsTestSim();
        sim.submit(CreateStockpileCommand{CellRect{2, 2, 1, 1, 0}});  // zone 1, the nearest
        if (another) {
            sim.submit(CreateStockpileCommand{CellRect{2, 28, 1, 1, 0}});  // zone 2
        }
        const Entity steel = jobsSpawn(sim, "Steel", CellPos{12, 6, 0}, 10);
        const Entity pawn = jobsSpawn(sim, "Human", CellPos{14, 6, 0});
        const entt::registry& reg = sim.things().registry();
        JOBS_REQUIRE(jobsRunUntil(sim, 3600, [&] { return reg.all_of<Carrying>(pawn); }));
        CHECK(jobOf(sim, pawn).cell == CellPos{2, 2, 0});
        sim.submit(RemoveZoneCellsCommand{CellRect{2, 2, 1, 1, 0}});
        sim.step();
        if (another) {
            CHECK(jobOf(sim, pawn).cell == CellPos{2, 28, 0});
            CHECK(jobsRunUntil(sim, 3600, [&] { return sim.itemsLeft() == 0; }));
            CHECK(sim.stats().failedJobs == 0);
        } else {
            CHECK(sim.stats().failedJobs == 1);
            CHECK_FALSE(reg.all_of<Carrying>(pawn));
            CHECK(sim.things().spawned(steel));
            CHECK(jobOf(sim, pawn).type == JobType::None);
            int failed = 0;
            for (const Event& e : sim.events()) {
                failed += e.type == EventType::JobEnded && e.message.rfind("failed: ", 0) == 0 ? 1 : 0;
            }
            CHECK(failed == 1);
        }
    }
}

TEST_CASE("Jobs: a haul order ends what the pawn is doing, and is refused when it can't be done") {
    Sim sim = jobsTestSim();
    sim.submit(CreateStockpileCommand{CellRect{2, 2, 1, 1, 0}});
    const Entity pawn = jobsSpawn(sim, "Human", CellPos{10, 10, 0});
    CHECK(jobOf(sim, pawn).type == JobType::Wait);  // nothing to do yet
    sim.submit(SetWorkPriorityCommand{pawn, "Hauling", 0});
    const Entity steel = jobsSpawn(sim, "Steel", CellPos{20, 10, 0}, 5);
    sim.submit(OrderHaulCommand{pawn, steel});
    sim.step();
    CHECK(jobsCount(sim, EventType::CommandFailed) == 0);
    const Job& job = jobOf(sim, pawn);
    CHECK(job.type == JobType::HaulToCell);
    CHECK(job.forced);
    CHECK_FALSE(job.workType.valid());
    bool interrupted = false;
    for (const Event& e : sim.events()) {
        interrupted = interrupted || (e.type == EventType::JobEnded && e.job == JobType::Wait &&
                                      e.message == "interrupted: ordered to haul");
    }
    CHECK(interrupted);
    CHECK(jobsRunUntil(sim, 3600, [&] { return sim.itemsLeft() == 0; }));  // even with hauling off

    jobsWallRing(sim, 25, 25);
    const Entity walledIn = jobsSpawn(sim, "Steel", CellPos{25, 25, 0});
    const Entity wood = jobsSpawn(sim, "WoodLog", CellPos{12, 12, 0});  // the one cell holds steel
    sim.submit(OrderHaulCommand{pawn, walledIn});
    sim.submit(OrderHaulCommand{pawn, wood});
    sim.submit(OrderHaulCommand{pawn, kNoEntity});
    sim.submit(OrderHaulCommand{steel, wood});
    sim.step();
    CHECK(jobsCount(sim, EventType::CommandFailed) == 4);
}

TEST_CASE("Jobs: the snapshot shows each pawn's job and load, and the stockpiles") {
    Sim sim = jobsTestSim();
    sim.submit(CreateStockpileCommand{CellRect{2, 2, 2, 3, 0}, StoragePriority::Important});
    const Entity steel = jobsSpawn(sim, "Steel", CellPos{20, 20, 0}, 7);
    const Entity pawn = jobsSpawn(sim, "Human", CellPos{18, 18, 0});
    Snapshot snap;
    sim.writeSnapshot(snap);
    JOBS_REQUIRE(snap.zones.size() == 1);
    CHECK(snap.zones[0].id == 1);
    CHECK(snap.zones[0].priority == StoragePriority::Important);
    CHECK(snap.zones[0].cellCount == 6);
    CHECK(snap.zoneCells.size() == 6);
    const ThingView* view = snap.find(pawn);
    JOBS_REQUIRE(view != nullptr);
    CHECK(view->job == JobType::HaulToCell);
    CHECK(view->jobTarget == steel);

    const entt::registry& reg = sim.things().registry();
    JOBS_REQUIRE(jobsRunUntil(sim, 3600, [&] { return reg.all_of<Carrying>(pawn); }));
    sim.writeSnapshot(snap);
    view = snap.find(pawn);
    JOBS_REQUIRE(view != nullptr);
    CHECK(view->carriedDef == sim.defs().idOf("Things", "Steel"));
    CHECK(view->carriedCount == 7);
    CHECK(snap.find(steel) == nullptr);  // in its hands, off the map
    CHECK(std::string(jobTypeName(view->job)) == "HaulToCell");
}

TEST_CASE("Jobs: a pawn walking to an item that was moved walks to where it is now") {
    Sim sim = jobsTestSim();
    sim.submit(CreateStockpileCommand{CellRect{2, 2, 1, 1, 0}});
    const Entity pawn = jobsSpawn(sim, "Human", CellPos{10, 10, 0});
    sim.submit(SetWorkPriorityCommand{pawn, "Hauling", 0});
    const Entity steel = jobsSpawn(sim, "Steel", CellPos{25, 10, 0}, 5);
    sim.submit(OrderHaulCommand{pawn, steel});
    sim.stepMany(40);
    JOBS_REQUIRE(jobOf(sim, pawn).type == JobType::HaulToCell);
    // Something (a wall going up) moves the stack while the pawn is on its way.
    sim.things().despawn(steel);
    const bool moved = sim.things().spawnAt(steel, CellPos{25, 14, 0});
    JOBS_REQUIRE(moved);
    CHECK(jobsRunUntil(sim, 3600, [&] { return sim.itemsLeft() == 0; }));
    CHECK(sim.stats().failedJobs == 0);
    CHECK(sim.stats().repaths >= 1);
}
