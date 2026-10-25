#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include "TestDefs.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define NEEDS_REQUIRE(...)     \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// Test defs with the needs rules (the defaults in code, which are the game's numbers), two kinds
// of food, a bed, a two-person bed, a table and a chair.
std::shared_ptr<const DefDatabase> needsTestDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        const std::vector<DefSource> sources = {
            {"Things", "test.json", R"([
        {"defName": "Human", "category": "Pawn", "stats": {"moveSpeed": 0.15333}, "race": {"foodPerDay": 1.6}},
        {"defName": "Steel", "category": "Item", "stackLimit": 75},
        {"defName": "Meal", "category": "Item", "stackLimit": 10, "stats": {"nutrition": 0.9},
         "ingestible": {"preferability": "MealSimple", "maxNumToIngestAtOnce": 1}},
        {"defName": "Berries", "category": "Item", "stackLimit": 75, "stats": {"nutrition": 0.05},
         "ingestible": {"preferability": "RawTasty"}},
        {"defName": "Gruel", "category": "Item", "stackLimit": 75, "stats": {"nutrition": 0.5},
         "ingestible": {"preferability": "DesperateOnly"}},
        {"defName": "Wall", "category": "Building", "passability": "Impassable", "fillPercent": 1.0},
        {"defName": "Bed", "category": "Building", "passability": "Standable", "stats": {"bedRestEffectiveness": 1.0},
         "building": {"bed": true, "sleepingSlots": 1}},
        {"defName": "DoubleBed", "category": "Building", "size": [2, 1], "passability": "Standable",
         "stats": {"bedRestEffectiveness": 1.0}, "building": {"bed": true, "sleepingSlots": 2}},
        {"defName": "Table", "category": "Building", "passability": "Impassable", "building": {"isTable": true}},
        {"defName": "Chair", "category": "Building", "passability": "Standable", "building": {"isSittable": true}}
    ])"},
            {"Terrain", "test.json", R"([{"defName": "Soil"}])"},
            {"Rules", "test.json", R"([{"defName": "NeedRules"}])"},
            {"WorkTypes", "test.json", R"([
        {"defName": "Hauling", "naturalPriority": 300, "tasks": ["HaulToStockpile"]}
    ])"}};
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::build(sources, "", *db, errors);
        return db;
    }();
    return defs;
}

// A flat 32 m map with the test defs and no hauling test.
Sim needsSim() { return Sim(SimSettings{5, 32, 0, 0, true}, needsTestDefs()); }

Entity needsSpawn(Sim& sim, const std::string& def, CellPos cell, int count = 1) {
    return sim.things().spawn(def, cell, count);
}

// Makes a pawn that works and has needs, as the spawn command does.
Entity needsPawn(Sim& sim, CellPos cell) {
    const Entity pawn = sim.things().spawn("Human", cell);
    sim.jobs().addWorker(pawn);
    return pawn;
}

Needs& needsOf(Sim& sim, Entity pawn) { return sim.things().registry().get<Needs>(pawn); }

JobType needsJob(Sim& sim, Entity pawn) { return sim.things().registry().get<Worker>(pawn).job.type; }

CellPos needsCell(Sim& sim, Entity pawn) { return cellOf(sim.things().registry().get<Position>(pawn)); }

struct NeedsLog {
    std::vector<Event> events;
    int count(EventType type, const std::string& message = std::string()) const {
        return static_cast<int>(std::count_if(events.begin(), events.end(), [&](const Event& e) {
            return e.type == type && (message.empty() || e.message == message);
        }));
    }
    const Event* first(EventType type) const {
        const auto it = std::find_if(events.begin(), events.end(), [&](const Event& e) { return e.type == type; });
        return it == events.end() ? nullptr : &*it;
    }
};

// Steps until `done` says so, at most `limit` steps, keeping the events. Returns whether it did.
template <typename F>
bool needsRunUntil(Sim& sim, NeedsLog& log, int limit, F done) {
    for (int i = 0; i < limit; ++i) {
        if (done()) {
            return true;
        }
        sim.step();
        log.events.insert(log.events.end(), sim.events().begin(), sim.events().end());
    }
    return done();
}

}  // namespace

TEST_CASE("Needs: food and rest fall at RimWorld's rates") {
    Sim sim = needsSim();
    const Entity pawn = needsPawn(sim, CellPos{5, 5, 0});
    NEEDS_REQUIRE(sim.things().registry().all_of<Needs, Schedule>(pawn));
    CHECK(needsOf(sim, pawn).food == doctest::Approx(1.0f));

    sim.stepMany(3600);  // one hour
    // 1.6 nutrition a day is 6.67% an hour; rest falls 95% a day, 3.96% an hour.
    CHECK(needsOf(sim, pawn).food == doctest::Approx(1.0f - 1.6f / 24.0f).epsilon(0.001));
    CHECK(needsOf(sim, pawn).rest == doctest::Approx(1.0f - 0.95f / 24.0f).epsilon(0.001));
    CHECK(sim.needs().rules().foodSeekBelow == doctest::Approx(0.3f));

    // Hungrier means slower: from 0.2 (the Hungry band) food falls at half the rate.
    needsOf(sim, pawn).food = 0.2f;
    const float before = needsOf(sim, pawn).food;
    sim.stepMany(3600);
    CHECK(before - needsOf(sim, pawn).food == doctest::Approx(0.8f / 24.0f).epsilon(0.01));

}

TEST_CASE("Needs: without a NeedRules def pawns have no needs, and the hauling test's pawns never have any") {
    Sim bare(SimSettings{5, 32, 0, 0, true}, testDefs());
    const Entity pawn = bare.things().spawn("Human", CellPos{5, 5, 0});
    bare.jobs().addWorker(pawn);
    CHECK_FALSE(bare.things().registry().all_of<Needs>(pawn));
    CHECK_FALSE(bare.things().registry().all_of<Schedule>(pawn));

    Sim hauling(SimSettings{5, 32, 2, 4, true}, needsTestDefs());
    for (const Entity hauler : hauling.haulers()) {
        CHECK_FALSE(hauling.things().registry().all_of<Needs>(hauler));
    }
}

TEST_CASE("Needs: a hungry pawn goes for the tastiest food it can reach and eats it") {
    Sim sim = needsSim();
    const Entity pawn = needsPawn(sim, CellPos{5, 5, 0});
    const Entity berries = needsSpawn(sim, "Berries", CellPos{7, 5, 0}, 40);  // nearer
    const Entity meal = needsSpawn(sim, "Meal", CellPos{20, 5, 0}, 3);        // tastier, further
    needsOf(sim, pawn).food = 0.2f;
    NeedsLog log;
    NEEDS_REQUIRE(needsRunUntil(sim, log, 300, [&] { return needsJob(sim, pawn) == JobType::Ingest; }));
    CHECK(sim.things().registry().get<Worker>(pawn).job.thing != berries);

    NEEDS_REQUIRE(needsRunUntil(sim, log, 3000, [&] { return log.count(EventType::PawnAte) == 1; }));
    const Event* ate = log.first(EventType::PawnAte);
    NEEDS_REQUIRE(ate != nullptr);
    CHECK(ate->thing == pawn);
    // One meal (a meal is eaten one at a time) fills the stomach: 0.2 and 0.9 make more than 1.
    CHECK(ate->amount == doctest::Approx(0.8f).epsilon(0.05));
    CHECK(ate->message == "ground");
    CHECK(needsOf(sim, pawn).food == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(sim.things().registry().get<StackCount>(meal).count == 2);
    CHECK(sim.things().registry().get<StackCount>(berries).count == 40);
    CHECK(log.count(EventType::PawnAction) == 2);  // pick up, eat
    CHECK(sim.stats().failedJobs == 0);
}

TEST_CASE("Needs: raw food is eaten by the handful, enough to fill the stomach") {
    Sim sim = needsSim();
    const Entity pawn = needsPawn(sim, CellPos{5, 5, 0});
    const Entity berries = needsSpawn(sim, "Berries", CellPos{6, 5, 0}, 75);
    needsOf(sim, pawn).food = 0.25f;
    NeedsLog log;
    NEEDS_REQUIRE(needsRunUntil(sim, log, 2000, [&] { return log.count(EventType::PawnAte) == 1; }));
    // 0.75 short of full is fifteen berries at 0.05 each.
    CHECK(log.first(EventType::PawnAte)->amount == doctest::Approx(0.75f).epsilon(0.02));
    CHECK(sim.things().registry().get<StackCount>(berries).count == 60);
    CHECK(needsOf(sim, pawn).food == doctest::Approx(1.0f).epsilon(0.01));
}

TEST_CASE("Needs: with a table and a chair beside it the pawn sits down to eat") {
    Sim sim = needsSim();
    const Entity pawn = needsPawn(sim, CellPos{5, 5, 0});
    needsSpawn(sim, "Meal", CellPos{6, 5, 0}, 2);
    needsSpawn(sim, "Table", CellPos{12, 5, 0});
    needsSpawn(sim, "Chair", CellPos{13, 5, 0});
    needsOf(sim, pawn).food = 0.1f;
    NeedsLog log;
    CellPos whereItAte;
    NEEDS_REQUIRE(needsRunUntil(sim, log, 3000, [&] {
        if (log.count(EventType::PawnAction) > 0) {
            for (const Event& e : log.events) {
                if (e.type == EventType::PawnAction && e.action == PawnAction::Eat) {
                    whereItAte = needsCell(sim, pawn);
                }
            }
        }
        return log.count(EventType::PawnAte) == 1;
    }));
    CHECK(whereItAte == CellPos{13, 5, 0});
    CHECK(log.first(EventType::PawnAte)->message == "table");

    // A chair away from every table is just a chair: the second pawn eats where the food lay.
    Sim other = needsSim();
    const Entity pawn2 = needsPawn(other, CellPos{5, 5, 0});
    needsSpawn(other, "Meal", CellPos{6, 5, 0}, 2);
    needsSpawn(other, "Chair", CellPos{13, 5, 0});
    needsOf(other, pawn2).food = 0.1f;
    NeedsLog log2;
    NEEDS_REQUIRE(needsRunUntil(other, log2, 3000, [&] { return log2.count(EventType::PawnAte) == 1; }));
    CHECK(log2.first(EventType::PawnAte)->message == "ground");
    CHECK(needsCell(other, pawn2) == CellPos{6, 5, 0});
}

TEST_CASE("Needs: two hungry pawns don't take the same food or the same chair") {
    Sim sim = needsSim();
    const Entity a = needsPawn(sim, CellPos{5, 5, 0});
    const Entity b = needsPawn(sim, CellPos{5, 7, 0});
    needsSpawn(sim, "Meal", CellPos{6, 6, 0}, 1);
    needsSpawn(sim, "Meal", CellPos{7, 6, 0}, 1);
    needsSpawn(sim, "Table", CellPos{12, 5, 0});
    needsSpawn(sim, "Chair", CellPos{13, 5, 0});
    needsSpawn(sim, "Chair", CellPos{11, 5, 0});
    needsOf(sim, a).food = 0.1f;
    needsOf(sim, b).food = 0.1f;
    NeedsLog log;
    NEEDS_REQUIRE(needsRunUntil(sim, log, 4000, [&] { return log.count(EventType::PawnAte) == 2; }));
    CHECK(log.count(EventType::PawnAte, "table") == 2);
    CHECK(sim.stats().failedJobs == 0);
}

TEST_CASE("Needs: desperate food is only for the ravenous, and no food means looking again later") {
    Sim sim = needsSim();
    const Entity pawn = needsPawn(sim, CellPos{5, 5, 0});
    needsSpawn(sim, "Gruel", CellPos{8, 5, 0}, 5);
    needsOf(sim, pawn).food = 0.2f;  // hungry, not ravenous
    sim.stepMany(200);
    CHECK(needsJob(sim, pawn) != JobType::Ingest);
    CHECK(needsOf(sim, pawn).foodSearchAfter > 0);  // it looked, found nothing, and waits a minute
    needsOf(sim, pawn).food = 0.1f;
    needsOf(sim, pawn).foodSearchAfter = 0;
    NeedsLog log;
    NEEDS_REQUIRE(needsRunUntil(sim, log, 500, [&] { return needsJob(sim, pawn) == JobType::Ingest; }));
}

TEST_CASE("Needs: a pawn hauling puts the job down to eat when it gets hungry") {
    Sim sim = needsSim();
    const Entity pawn = needsPawn(sim, CellPos{5, 5, 0});
    sim.submit(CreateStockpileCommand{CellRect{25, 25, 3, 3, 0}, StoragePriority::Normal});
    needsSpawn(sim, "Steel", CellPos{6, 6, 0}, 10);
    needsSpawn(sim, "Meal", CellPos{4, 5, 0}, 1);
    NeedsLog log;
    NEEDS_REQUIRE(needsRunUntil(sim, log, 200, [&] { return needsJob(sim, pawn) == JobType::HaulToCell; }));
    needsOf(sim, pawn).food = 0.2f;
    NEEDS_REQUIRE(needsRunUntil(sim, log, 50, [&] { return needsJob(sim, pawn) == JobType::Ingest; }));
    const auto ended = std::find_if(log.events.begin(), log.events.end(), [](const Event& e) {
        return e.type == EventType::JobEnded && e.job == JobType::HaulToCell;
    });
    NEEDS_REQUIRE(ended != log.events.end());
    CHECK(ended->message == "interrupted: hungry");
    // After eating it goes back to hauling.
    NEEDS_REQUIRE(needsRunUntil(sim, log, 4000, [&] { return log.count(EventType::PawnAte) == 1; }));
    NEEDS_REQUIRE(needsRunUntil(sim, log, 4000, [&] { return sim.itemsLeft() == 0; }));
}

TEST_CASE("Needs: a tired pawn in a Sleep hour goes to bed, claims it, sleeps and wakes rested") {
    Sim sim = needsSim();
    const Entity pawn = needsPawn(sim, CellPos{5, 5, 0});
    const Entity far = needsSpawn(sim, "Bed", CellPos{20, 20, 0});
    const Entity near = needsSpawn(sim, "Bed", CellPos{8, 5, 0});
    needsOf(sim, pawn).rest = 0.5f;  // game second 0 is midnight: a Sleep hour
    NeedsLog log;
    NEEDS_REQUIRE(needsRunUntil(sim, log, 300, [&] { return needsJob(sim, pawn) == JobType::LayDown; }));
    CHECK(sim.jobs().bedOf(pawn) == near);
    CHECK(sim.things().registry().get<Bed>(far).owners.empty());

    NEEDS_REQUIRE(needsRunUntil(sim, log, 600, [&] { return jobIsLyingDown(sim.things().registry().get<Worker>(pawn).job); }));
    CHECK(needsCell(sim, pawn) == CellPos{8, 5, 0});
    const float lying = needsOf(sim, pawn).rest;
    sim.stepMany(3600);
    // A bed gives 2.2857 a day: 9.5% an hour.
    CHECK(needsOf(sim, pawn).rest - lying == doctest::Approx(2.285714f / 24.0f).epsilon(0.01));
    Snapshot snapshot;
    sim.writeSnapshot(snapshot);
    NEEDS_REQUIRE(snapshot.find(pawn) != nullptr);
    CHECK(snapshot.find(pawn)->asleep);
    CHECK(snapshot.find(pawn)->rest == doctest::Approx(needsOf(sim, pawn).rest));

    // Rested at 1.0 (about five hours), it wakes by itself.
    NEEDS_REQUIRE(needsRunUntil(sim, log, 20000, [&] { return log.count(EventType::PawnWokeUp) == 1; }));
    CHECK(log.first(EventType::PawnWokeUp)->message == "bed");
    CHECK(needsOf(sim, pawn).rest >= 0.999f);
    CHECK(needsJob(sim, pawn) != JobType::LayDown);
    CHECK(sim.stats().failedJobs == 0);
}

TEST_CASE("Needs: with no bed free a pawn sleeps on the ground where it stands") {
    Sim sim = needsSim();
    const Entity a = needsPawn(sim, CellPos{5, 5, 0});
    const Entity b = needsPawn(sim, CellPos{5, 9, 0});
    const Entity c = needsPawn(sim, CellPos{9, 9, 0});
    const Entity bed = needsSpawn(sim, "Bed", CellPos{5, 6, 0});
    const Entity doubleBed = needsSpawn(sim, "DoubleBed", CellPos{12, 12, 0});  // two slots
    for (const Entity pawn : {a, b, c}) {
        needsOf(sim, pawn).rest = 0.4f;
    }
    NeedsLog log;
    NEEDS_REQUIRE(needsRunUntil(sim, log, 60, [&] {
        return needsJob(sim, a) == JobType::LayDown && needsJob(sim, b) == JobType::LayDown && needsJob(sim, c) == JobType::LayDown;
    }));
    // Three pawns, three slots: each owns a different one. A fourth would sleep on the ground.
    const Entity d = needsPawn(sim, CellPos{14, 14, 0});
    needsOf(sim, d).rest = 0.4f;
    NEEDS_REQUIRE(needsRunUntil(sim, log, 60, [&] { return needsJob(sim, d) == JobType::LayDown; }));
    CHECK(sim.jobs().bedOf(d) == kNoEntity);
    CHECK(sim.things().registry().get<Worker>(d).job.thing == kNoEntity);
    CHECK(sim.things().registry().get<Bed>(bed).owners.size() == 1);
    CHECK(sim.things().registry().get<Bed>(doubleBed).owners.size() == 2);
    NEEDS_REQUIRE(needsRunUntil(sim, log, 200, [&] { return jobIsLyingDown(sim.things().registry().get<Worker>(d).job); }));
    CHECK(needsCell(sim, d) == CellPos{14, 14, 0});
    // On the ground it rests at 80%.
    const float lying = needsOf(sim, d).rest;
    sim.stepMany(3600);
    CHECK(needsOf(sim, d).rest - lying == doctest::Approx(0.8f * 2.285714f / 24.0f).epsilon(0.02));
}

TEST_CASE("Needs: bed ownership by command: assign, take over, unassign, full and bad targets") {
    Sim sim = needsSim();
    const Entity a = needsPawn(sim, CellPos{5, 5, 0});
    const Entity b = needsPawn(sim, CellPos{5, 7, 0});
    const Entity bed1 = needsSpawn(sim, "Bed", CellPos{6, 5, 0});
    const Entity bed2 = needsSpawn(sim, "Bed", CellPos{20, 20, 0});
    const Entity wall = needsSpawn(sim, "Wall", CellPos{9, 9, 0});

    const auto run = [&](Command command) {
        sim.submit(std::move(command));
        sim.step();
        return std::count_if(sim.events().begin(), sim.events().end(),
                             [](const Event& e) { return e.type == EventType::CommandFailed; }) == 0;
    };
    CHECK(run(AssignBedCommand{bed1, a}));
    CHECK(sim.jobs().bedOf(a) == bed1);
    CHECK_FALSE(run(AssignBedCommand{bed1, b}));  // full
    CHECK(run(AssignBedCommand{bed2, a}));       // a moves to the other bed
    CHECK(sim.jobs().bedOf(a) == bed2);
    CHECK(sim.things().registry().get<Bed>(bed1).owners.empty());
    CHECK_FALSE(run(AssignBedCommand{wall, a}));      // not a bed
    CHECK_FALSE(run(AssignBedCommand{bed1, wall}));   // not a pawn who sleeps
    CHECK(run(AssignBedCommand{bed2, kNoEntity}));    // unassign
    CHECK(sim.jobs().bedOf(a) == kNoEntity);

    // The owner goes to its own bed even though another is nearer.
    CHECK(run(AssignBedCommand{bed2, a}));
    needsOf(sim, a).rest = 0.3f;
    NeedsLog log;
    NEEDS_REQUIRE(needsRunUntil(sim, log, 300, [&] { return needsJob(sim, a) == JobType::LayDown; }));
    CHECK(sim.things().registry().get<Worker>(a).job.thing == bed2);
    CHECK(sim.things().registry().get<Bed>(bed1).owners.empty());

    // Destroy the bed under a sleeper: the job fails and the pawn sleeps elsewhere.
    sim.submit(DestroyThingCommand{bed2});
    sim.stepMany(3);
    // The job failed ("the bed is gone"); the pawn thinks again and takes the free bed.
    CHECK(sim.jobs().bedOf(a) == bed1);
    NEEDS_REQUIRE(needsRunUntil(sim, log, 300, [&] { return needsJob(sim, a) == JobType::LayDown; }));
    CHECK(sim.things().registry().get<Worker>(a).job.thing == bed1);
}

TEST_CASE("Needs: the schedule: Work hours keep a drowsy pawn at work, Joy hours stop work, bad hours fail") {
    Sim sim = needsSim();
    const Entity pawn = needsPawn(sim, CellPos{5, 5, 0});
    const auto run = [&](Command command) {
        sim.submit(std::move(command));
        sim.step();
        return std::count_if(sim.events().begin(), sim.events().end(),
                             [](const Event& e) { return e.type == EventType::CommandFailed; }) == 0;
    };
    CHECK(sim.things().registry().get<Schedule>(pawn).hours[0] == ScheduleKind::Sleep);
    CHECK(sim.things().registry().get<Schedule>(pawn).hours[5] == ScheduleKind::Sleep);
    CHECK(sim.things().registry().get<Schedule>(pawn).hours[6] == ScheduleKind::Anything);
    CHECK(sim.things().registry().get<Schedule>(pawn).hours[21] == ScheduleKind::Anything);
    CHECK(sim.things().registry().get<Schedule>(pawn).hours[22] == ScheduleKind::Sleep);
    CHECK_FALSE(run(SetScheduleCommand{pawn, 24, ScheduleKind::Work}));
    CHECK_FALSE(run(SetScheduleCommand{pawn, -1, ScheduleKind::Work}));
    CHECK_FALSE(run(SetScheduleCommand{kNoEntity, 0, ScheduleKind::Work}));
    CHECK(run(SetScheduleCommand{pawn, 0, ScheduleKind::Work}));
    CHECK(sim.things().registry().get<Schedule>(pawn).hours[0] == ScheduleKind::Work);

    // Hour 0 is Work now: a pawn at 0.2 rest (drowsy) keeps working; at 0.1 (tired) it sleeps.
    needsSpawn(sim, "Steel", CellPos{6, 6, 0}, 5);
    sim.submit(CreateStockpileCommand{CellRect{25, 25, 3, 3, 0}, StoragePriority::Normal});
    needsOf(sim, pawn).rest = 0.2f;
    sim.stepMany(5);
    CHECK(needsJob(sim, pawn) != JobType::LayDown);
    needsOf(sim, pawn).rest = 0.1f;
    NeedsLog log;
    NEEDS_REQUIRE(needsRunUntil(sim, log, 50, [&] { return needsJob(sim, pawn) == JobType::LayDown; }));

    // A Joy hour: no hauling, the pawn just idles.
    Sim joy = needsSim();
    const Entity idler = needsPawn(joy, CellPos{5, 5, 0});
    for (int h = 0; h < 24; ++h) {
        joy.submit(SetScheduleCommand{idler, h, ScheduleKind::Joy});
    }
    needsSpawn(joy, "Steel", CellPos{6, 6, 0}, 5);
    joy.submit(CreateStockpileCommand{CellRect{25, 25, 3, 3, 0}, StoragePriority::Normal});
    joy.stepMany(600);
    CHECK(joy.stats().hauls == 0);
    CHECK(needsJob(joy, idler) == JobType::Wait);
}

TEST_CASE("Needs: save and load keep needs, schedules, bed owners and the eating and sleeping jobs") {
    const auto defs = needsTestDefs();
    Sim original(SimSettings{9, 32, 0, 0, true}, defs);
    const Entity eater = needsPawn(original, CellPos{5, 5, 0});
    const Entity sleeper = needsPawn(original, CellPos{5, 9, 0});
    needsSpawn(original, "Meal", CellPos{9, 5, 0}, 3);
    needsSpawn(original, "Table", CellPos{14, 5, 0});
    needsSpawn(original, "Chair", CellPos{15, 5, 0});
    needsSpawn(original, "Bed", CellPos{7, 9, 0});
    original.submit(SetScheduleCommand{sleeper, 3, ScheduleKind::Joy});
    needsOf(original, eater).food = 0.15f;
    needsOf(original, sleeper).rest = 0.4f;

    // Save in the middle of eating and in the middle of sleeping.
    NeedsLog log;
    NEEDS_REQUIRE(needsRunUntil(original, log, 3000, [&] {
        return needsJob(original, eater) == JobType::Ingest && original.things().registry().get<Worker>(eater).job.step >= 2 &&
               jobIsLyingDown(original.things().registry().get<Worker>(sleeper).job);
    }));
    const std::vector<uint8_t> bytes = saveSim(original);
    SaveLoadResult loaded = loadSim(bytes, defs);
    NEEDS_REQUIRE(loaded.error.empty());
    NEEDS_REQUIRE(loaded.sim != nullptr);
    Sim& copy = *loaded.sim;
    CHECK(copy.stateHash() == original.stateHash());
    CHECK(needsOf(copy, eater).food == needsOf(original, eater).food);
    CHECK(copy.jobs().bedOf(sleeper) == original.jobs().bedOf(sleeper));
    CHECK(copy.things().registry().get<Schedule>(sleeper).hours[3] == ScheduleKind::Joy);

    // On it goes the same: they eat and wake together.
    original.stepMany(20000);
    copy.stepMany(20000);
    CHECK(copy.stateHash() == original.stateHash());
    CHECK(needsOf(original, eater).food > 0.5f);
    CHECK(saveSim(copy) == saveSim(original));
}

TEST_CASE("Needs: the game's real numbers: starvation kills, and sleeping puts a hurt pawn on the bed's healing") {
    static const std::shared_ptr<const DefDatabase> real = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return db;
    }();
    Sim sim(SimSettings{3, 64, 0, 0, true}, real);
    CHECK(sim.needs().rules().present);
    CHECK(sim.needs().rules().restFallPerDayRested == doctest::Approx(0.95f));
    const Entity pawn = sim.things().spawn("Human", CellPos{10, 10, 0});
    sim.jobs().addWorker(pawn);
    NEEDS_REQUIRE(sim.things().registry().all_of<Needs, Health>(pawn));

    // Empty stomach, nothing to eat: malnutrition builds at 48% a day and kills at 100%.
    needsOf(sim, pawn).food = 0.0f;
    sim.stepMany(3600);
    CHECK(needsOf(sim, pawn).malnutrition == doctest::Approx(0.48f / 24.0f).epsilon(0.01));
    needsOf(sim, pawn).malnutrition = 0.9999f;
    NeedsLog log;
    NEEDS_REQUIRE(needsRunUntil(sim, log, 100, [&] { return log.count(EventType::PawnDied) == 1; }));
    CHECK(log.first(EventType::PawnDied)->message == "starvation");
    CHECK(sim.things().registry().get<Health>(pawn).cause == DeathCause::Starvation);
    CHECK(sim.things().registry().get<Health>(pawn).dead);

    // A tired pawn with no bed sleeps on the ground, which heals at the ground rate (HealthRules).
    const Entity sleeper = sim.things().spawn("Human", CellPos{30, 30, 0});
    sim.jobs().addWorker(sleeper);
    needsOf(sim, sleeper).rest = 0.3f;  // game second 0 is midnight, a Sleep hour
    NEEDS_REQUIRE(needsRunUntil(sim, log, 100, [&] { return jobIsLyingDown(sim.things().registry().get<Worker>(sleeper).job); }));
    CHECK(sim.things().registry().get<Health>(sleeper).rest == RestKind::Ground);

    // Eating raises it again: a survival meal fills a hungry pawn.
    const Entity eater = sim.things().spawn("Human", CellPos{20, 20, 0});
    sim.jobs().addWorker(eater);
    sim.things().spawn("MealSurvivalPack", CellPos{22, 20, 0}, 2);
    needsOf(sim, eater).food = 0.1f;
    NEEDS_REQUIRE(needsRunUntil(sim, log, 4000, [&] { return log.count(EventType::PawnAte) == 1; }));
    CHECK(needsOf(sim, eater).food == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(needsOf(sim, eater).food <= 1.0f);
}
